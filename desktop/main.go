// katana-desktop: janela de conversa com o agente do katana. Camada acima do motor:
// sobe `katana agent <modelo>` como processo filho, traduz a saida (marcadores, XML de
// ferramentas, estatisticas) para eventos de interface e serve uma pagina local.
// O usuario ve so texto e cartoes de ferramenta; XML e JSON ficam so no canal interno.
package main

import (
	"bufio"
	"embed"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"runtime"
	"sort"
	"strings"
	"sync"
	"time"
)

//go:embed index.html
var assets embed.FS

type Event struct {
	T    string `json:"t"`              // text, tool, ready, status, stats, error
	Text string `json:"text,omitempty"` // conteudo
	Name string `json:"name,omitempty"`
}

type ModelInfo struct {
	File      string  `json:"file"`
	Name      string  `json:"name"`
	SizeGB    float64 `json:"size_gb"`
	Usable    bool    `json:"usable"`
	Note      string  `json:"note,omitempty"`
	IsDefault bool    `json:"is_default"`
}

type App struct {
	katana, modelsDir string
	mu                sync.Mutex
	cmd               *exec.Cmd
	stdin             io.WriteCloser
	model             string
	subs              map[chan Event]struct{}
	hist              []Event // historico da conversa atual (para quem reconecta)
	busy              bool
	ready             bool
}

func (a *App) emit(e Event) {
	a.mu.Lock()
	if e.T == "text" || e.T == "tool" || e.T == "user" {
		a.hist = append(a.hist, e)
	}
	for c := range a.subs {
		select {
		case c <- e:
		default:
		}
	}
	a.mu.Unlock()
}

// ---- modelos ----

func usable(name string) (bool, string) {
	l := strings.ToLower(name)
	switch {
	case strings.Contains(l, "qwen3.6"):
		return true, ""
	case strings.Contains(l, "gemma-4") || strings.Contains(l, "gemma4"):
		return true, ""
	}
	return false, "ainda nao esta utilizavel no agente"
}

func (a *App) listModels() []ModelInfo {
	var out []ModelInfo
	files, _ := filepath.Glob(filepath.Join(a.modelsDir, "*.gguf"))
	for _, f := range files {
		st, err := os.Stat(f)
		if err != nil {
			continue
		}
		base := filepath.Base(f)
		ok, note := usable(base)
		out = append(out, ModelInfo{File: base, Name: strings.TrimSuffix(base, ".gguf"),
			SizeGB: float64(st.Size()) / 1073741824, Usable: ok, Note: note,
			IsDefault: strings.Contains(strings.ToLower(base), "qwen3.6")})
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Usable != out[j].Usable {
			return out[i].Usable
		}
		if out[i].IsDefault != out[j].IsDefault {
			return out[i].IsDefault
		}
		return out[i].Name < out[j].Name
	})
	return out
}

// ---- processo do agente ----

func (a *App) stop() {
	a.mu.Lock()
	cmd, in := a.cmd, a.stdin
	a.cmd, a.stdin, a.ready, a.busy = nil, nil, false, false
	a.mu.Unlock()
	if cmd == nil {
		return
	}
	if in != nil {
		io.WriteString(in, "/sair\n")
		in.Close()
	}
	done := make(chan struct{})
	go func() { cmd.Wait(); close(done) }()
	select {
	case <-done:
	case <-time.After(3 * time.Second):
		cmd.Process.Kill()
		<-done
	}
}

func (a *App) start(model string) error {
	path := filepath.Join(a.modelsDir, filepath.Base(model))
	if _, err := os.Stat(path); err != nil {
		return fmt.Errorf("modelo nao encontrado: %s", model)
	}
	a.stop()
	cmd := exec.Command(a.katana, "agent", path)
	cmd.Env = append(os.Environ(), "KATANA_UI=1", "NO_COLOR=1")
	if os.Getenv("KATANA_THINK") == "" {
		cmd.Env = append(cmd.Env, "KATANA_THINK=0")
	}
	prepProc(cmd)
	in, _ := cmd.StdinPipe()
	so, _ := cmd.StdoutPipe()
	se, _ := cmd.StderrPipe()
	if err := cmd.Start(); err != nil {
		return err
	}
	a.mu.Lock()
	a.cmd, a.stdin, a.model, a.hist, a.ready, a.busy = cmd, in, model, nil, false, true
	a.mu.Unlock()
	a.emit(Event{T: "status", Text: "Carregando o modelo..."})
	go a.readStdout(cmd, so)
	go a.readStderr(cmd, se)
	go func() {
		cmd.Wait()
		a.mu.Lock()
		cur := a.cmd == cmd
		if cur {
			a.cmd, a.stdin, a.ready, a.busy = nil, nil, false, false
		}
		a.mu.Unlock()
		if cur {
			a.emit(Event{T: "error", Text: "O agente terminou. Escolha um modelo para iniciar de novo."})
		}
	}()
	return nil
}

// ---- traducao da saida ----

var tagOpen = []string{"<tool_call>", "<|tool_call>", "<tool_response>", "<|tool_response>"}
var tagClose = map[string]string{
	"<tool_call>": "</tool_call>", "<|tool_call>": "<tool_call|>",
	"<tool_response>": "</tool_response>", "<|tool_response>": "<tool_response|>",
}

// filtro: remove blocos de chamada de ferramenta do texto de resposta
type filter struct {
	buf    string
	closer string // != "" dentro de um bloco oculto
}

func (f *filter) feed(s string) string {
	f.buf += s
	var out strings.Builder
	for {
		if f.closer != "" {
			i := strings.Index(f.buf, f.closer)
			if i < 0 {
				// guarda so o que pode ser inicio do fechamento
				keep := len(f.closer) - 1
				if len(f.buf) > keep {
					f.buf = f.buf[len(f.buf)-keep:]
				}
				return out.String()
			}
			f.buf = f.buf[i+len(f.closer):]
			f.closer = ""
			continue
		}
		best, bi := "", -1
		for _, t := range tagOpen {
			if i := strings.Index(f.buf, t); i >= 0 && (bi < 0 || i < bi) {
				best, bi = t, i
			}
		}
		if bi >= 0 {
			out.WriteString(f.buf[:bi])
			f.buf = f.buf[bi+len(best):]
			f.closer = tagClose[best]
			continue
		}
		// segura sufixo que pode ser comeco de uma etiqueta
		cut := len(f.buf)
		for k := 1; k < 20 && k <= len(f.buf); k++ {
			suf := f.buf[len(f.buf)-k:]
			for _, t := range tagOpen {
				if strings.HasPrefix(t, suf) {
					cut = len(f.buf) - k
				}
			}
		}
		out.WriteString(f.buf[:cut])
		f.buf = f.buf[cut:]
		return out.String()
	}
}

func (f *filter) flush() string {
	s := f.buf
	f.buf = ""
	if f.closer != "" {
		return ""
	}
	return s
}

func (a *App) readStdout(cmd *exec.Cmd, r io.Reader) {
	br := bufio.NewReaderSize(r, 1<<16)
	var f filter
	var acc strings.Builder
	atLineStart := true
	var pending string // linha parcial que pode ser marcador
	for {
		b, err := br.ReadByte()
		if err != nil {
			return
		}
		a.mu.Lock()
		cur := a.cmd == cmd
		a.mu.Unlock()
		if !cur {
			return
		}
		if atLineStart && b == 0x01 { // marcador do katana
			line, _ := br.ReadString('\n')
			line = strings.TrimRight(line, "\r\n")
			if acc.Len() > 0 {
				a.emitUTF8(acc.String())
				acc.Reset()
			}
			if s := f.flush(); s != "" {
				a.emitUTF8(s)
			}
			f = filter{}
			switch {
			case line == "READY":
				a.mu.Lock()
				a.ready, a.busy = true, false
				a.mu.Unlock()
				a.emit(Event{T: "ready"})
			case strings.HasPrefix(line, "TOOL "):
				p := strings.SplitN(line[5:], "\t", 2)
				e := Event{T: "tool", Name: p[0]}
				if len(p) > 1 {
					e.Text = cleanArgs(p[1])
				}
				a.emit(e)
			}
			atLineStart = true
			continue
		}
		_ = pending
		acc.WriteString(f.feed(string([]byte{b})))
		atLineStart = b == '\n'
		if br.Buffered() == 0 && acc.Len() > 0 { // fim do bloco lido: emite junto
			a.emitUTF8(acc.String())
			acc.Reset()
		}
	}
}

var utf8Tail []byte

func (a *App) emitUTF8(s string) {
	b := append(utf8Tail, s...)
	utf8Tail = nil
	// separa sufixo incompleto
	for k := 1; k <= 3 && k <= len(b); k++ {
		c := b[len(b)-k]
		if c&0xC0 == 0x80 {
			continue
		}
		need := 1
		switch {
		case c >= 0xF0:
			need = 4
		case c >= 0xE0:
			need = 3
		case c >= 0xC0:
			need = 2
		}
		if need > k {
			utf8Tail = append([]byte{}, b[len(b)-k:]...)
			b = b[:len(b)-k]
		}
		break
	}
	t := string(b)
	t = strings.ReplaceAll(t, "[raciocinio]\n", "")
	if t != "" {
		a.emit(Event{T: "text", Text: t})
	}
}

var jsonKV = regexp.MustCompile(`"(\w+)"\s*:\s*"((?:[^"\\]|\\.)*)"`)

// transforma os argumentos crus (JSON/XML) em texto legivel
func cleanArgs(s string) string {
	s = strings.TrimSpace(s)
	if m := jsonKV.FindAllStringSubmatch(s, -1); len(m) > 0 && strings.Contains(s, "{") {
		var parts []string
		for _, x := range m {
			v := strings.NewReplacer(`\"`, `"`, `\\n`, " ", `\n`, " ", `\\`, `\`).Replace(x[2])
			parts = append(parts, x[1]+": "+v)
		}
		return strings.Join(parts, " | ")
	}
	s = regexp.MustCompile(`</?[a-z_=|]+[^>]*>`).ReplaceAllString(s, " ")
	return strings.TrimSpace(s)
}

var statsRe = regexp.MustCompile(`\[entrada: (\d+) tokens, ([\d.]+) tok/s \| resposta: (\d+) tokens, ([\d.]+) tok/s \| contexto (\d+) de (\d+)`)

func (a *App) readStderr(cmd *exec.Cmd, r io.Reader) {
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 1<<16), 1<<20)
	sc.Split(func(d []byte, eof bool) (int, []byte, error) { // linhas e o prompt "voce> " sem \n
		for i, c := range d {
			if c == '\n' {
				return i + 1, d[:i], nil
			}
		}
		if eof && len(d) > 0 {
			return len(d), d, nil
		}
		return 0, nil, nil
	})
	for sc.Scan() {
		l := strings.TrimSpace(sc.Text())
		fmt.Fprintln(os.Stderr, "[katana]", l)
		if m := statsRe.FindStringSubmatch(l); m != nil {
			a.emit(Event{T: "stats", Text: fmt.Sprintf("%s tok/s | contexto %s de %s", m[4], m[5], m[6])})
		} else if strings.Contains(l, "contexto cheio") {
			a.emit(Event{T: "error", Text: "Contexto cheio. Inicie uma conversa nova."})
		} else if strings.Contains(l, "interrompido") {
			a.emit(Event{T: "status", Text: "Interrompido"})
		}
	}
}

// ---- HTTP ----

func (a *App) routes() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/" {
			http.NotFound(w, r)
			return
		}
		b, _ := assets.ReadFile("index.html")
		w.Header().Set("Content-Type", "text/html; charset=utf-8")
		w.Write(b)
	})
	mux.HandleFunc("/api/models", func(w http.ResponseWriter, r *http.Request) {
		a.mu.Lock()
		cur, ready := a.model, a.ready
		a.mu.Unlock()
		json.NewEncoder(w).Encode(map[string]any{"models": a.listModels(), "current": cur, "ready": ready})
	})
	mux.HandleFunc("/api/start", func(w http.ResponseWriter, r *http.Request) {
		var q struct{ Model string }
		json.NewDecoder(r.Body).Decode(&q)
		if err := a.start(q.Model); err != nil {
			http.Error(w, err.Error(), 400)
			return
		}
		w.Write([]byte("ok"))
	})
	mux.HandleFunc("/api/send", func(w http.ResponseWriter, r *http.Request) {
		var q struct{ Text string }
		json.NewDecoder(r.Body).Decode(&q)
		text := strings.Join(strings.Fields(strings.ReplaceAll(q.Text, "\n", " ")), " ") // o agente le uma linha por turno
		a.mu.Lock()
		in, ready := a.stdin, a.ready
		if in != nil && ready && text != "" {
			a.ready, a.busy = false, true
		} else {
			in = nil
		}
		a.mu.Unlock()
		if in == nil {
			http.Error(w, "agente ocupado ou nao iniciado", 409)
			return
		}
		a.emit(Event{T: "user", Text: q.Text})
		io.WriteString(in, text+"\n")
		w.Write([]byte("ok"))
	})
	mux.HandleFunc("/api/stop", func(w http.ResponseWriter, r *http.Request) {
		a.mu.Lock()
		cmd := a.cmd
		a.mu.Unlock()
		if cmd != nil && cmd.Process != nil {
			interrupt(cmd)
		}
		w.Write([]byte("ok"))
	})
	mux.HandleFunc("/api/new", func(w http.ResponseWriter, r *http.Request) {
		a.mu.Lock()
		in, ready := a.stdin, a.ready
		if ready && in != nil {
			a.hist = nil
			a.ready = false
		} else {
			in = nil
		}
		a.mu.Unlock()
		if in == nil {
			http.Error(w, "ocupado", 409)
			return
		}
		io.WriteString(in, "/novo\n")
		w.Write([]byte("ok"))
	})
	mux.HandleFunc("/api/events", func(w http.ResponseWriter, r *http.Request) {
		fl, _ := w.(http.Flusher)
		w.Header().Set("Content-Type", "text/event-stream")
		w.Header().Set("Cache-Control", "no-store")
		ch := make(chan Event, 256)
		a.mu.Lock()
		hist := append([]Event{}, a.hist...)
		ready, started := a.ready, a.cmd != nil
		a.subs[ch] = struct{}{}
		a.mu.Unlock()
		defer func() { a.mu.Lock(); delete(a.subs, ch); a.mu.Unlock() }()
		send := func(e Event) { b, _ := json.Marshal(e); fmt.Fprintf(w, "data: %s\n\n", b); fl.Flush() }
		for _, e := range hist {
			send(e)
		}
		if ready {
			send(Event{T: "ready"})
		} else if started {
			send(Event{T: "status", Text: "Trabalhando..."})
		}
		tick := time.NewTicker(20 * time.Second)
		defer tick.Stop()
		for {
			select {
			case e := <-ch:
				send(e)
			case <-tick.C:
				fmt.Fprint(w, ": ping\n\n")
				fl.Flush()
			case <-r.Context().Done():
				return
			}
		}
	})
	return mux
}

func openWindow(url string) {
	var cands [][]string
	switch runtime.GOOS {
	case "windows":
		cands = [][]string{{"cmd", "/c", "start", "", url}}
	case "darwin":
		cands = [][]string{{"open", url}}
	default:
		for _, b := range []string{"google-chrome", "chromium", "chromium-browser", "microsoft-edge", "brave-browser"} {
			cands = append(cands, []string{b, "--app=" + url, "--window-size=1000,780"})
		}
		cands = append(cands, []string{"xdg-open", url})
	}
	for _, c := range cands {
		if p, err := exec.LookPath(c[0]); err == nil {
			cmd := exec.Command(p, c[1:]...)
			if cmd.Start() == nil {
				go cmd.Wait()
				return
			}
		}
	}
	fmt.Println("Abra no navegador:", url)
}

func main() {
	exe, _ := os.Executable()
	dir := filepath.Dir(exe)
	katana := flag.String("katana", "", "caminho do executavel katana")
	models := flag.String("models", "", "pasta dos modelos (.gguf)")
	port := flag.Int("port", 8787, "porta local")
	nowin := flag.Bool("no-window", false, "nao abre a janela")
	flag.Parse()
	pick := func(v string, cands ...string) string {
		if v != "" {
			return v
		}
		for _, c := range cands {
			if _, err := os.Stat(c); err == nil {
				return c
			}
		}
		return cands[0]
	}
	cwd, _ := os.Getwd()
	app := &App{subs: map[chan Event]struct{}{}}
	app.katana = pick(*katana, filepath.Join(dir, "katana"), filepath.Join(dir, "..", "katana"), filepath.Join(cwd, "katana"), filepath.Join(cwd, "..", "katana"))
	app.modelsDir = pick(*models, filepath.Join(dir, "modelos"), filepath.Join(dir, "..", "modelos"), filepath.Join(cwd, "modelos"), filepath.Join(cwd, "..", "modelos"))
	if _, err := os.Stat(app.katana); err != nil {
		fmt.Fprintln(os.Stderr, "katana nao encontrado; use -katana CAMINHO")
		os.Exit(1)
	}
	ln, err := net.Listen("tcp", fmt.Sprintf("127.0.0.1:%d", *port))
	if err != nil {
		ln, err = net.Listen("tcp", "127.0.0.1:0")
		if err != nil {
			fmt.Fprintln(os.Stderr, err)
			os.Exit(1)
		}
	}
	url := "http://" + ln.Addr().String()
	fmt.Println("katana-desktop:", url, "| katana:", app.katana, "| modelos:", app.modelsDir)
	if !*nowin {
		openWindow(url)
	}
	defer app.stop()
	srv := &http.Server{Handler: guard(app.routes())}
	srv.Serve(ln)
}

// so a propria maquina: recusa Host/Origin que nao sejam locais
func guard(h http.Handler) http.Handler {
	local := func(s string) bool {
		return strings.HasPrefix(s, "127.0.0.1:") || strings.HasPrefix(s, "localhost:") || s == "127.0.0.1" || s == "localhost"
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if !local(r.Host) {
			http.Error(w, "host nao permitido", 403)
			return
		}
		if o := r.Header.Get("Origin"); o != "" {
			if i := strings.Index(o, "://"); i < 0 || !local(o[i+3:]) {
				http.Error(w, "origem nao permitida", 403)
				return
			}
		}
		h.ServeHTTP(w, r)
	})
}
