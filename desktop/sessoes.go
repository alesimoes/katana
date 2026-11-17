package main

// Persistencia das conversas: um arquivo JSON por sessao em ~/.katana/desktop/sessoes.

import (
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"sort"
	"strings"
	"time"
	"unicode/utf8"
)

type Msg struct {
	Papel string `json:"papel"` // user, assistant, tool
	Texto string `json:"texto"`
	Nome  string `json:"nome,omitempty"`
}

type Session struct {
	ID         string `json:"id"`
	Titulo     string `json:"titulo"`
	Modelo     string `json:"modelo"`
	Criada     string `json:"criada"`
	Atualizada string `json:"atualizada"`
	Mensagens  []Msg  `json:"mensagens"`
}

type SessionInfo struct {
	ID         string `json:"id"`
	Titulo     string `json:"titulo"`
	Modelo     string `json:"modelo"`
	Criada     string `json:"criada"`
	Atualizada string `json:"atualizada"`
	Mensagens  int    `json:"mensagens"`
	Atual      bool   `json:"atual"`
}

const maxContexto = 6000 // caracteres do resumo enviado ao agente ao retomar uma sessao

var idRe = regexp.MustCompile(`^[A-Za-z0-9_-]{1,64}$`)

func validID(id string) bool { return idRe.MatchString(id) }

func newSession(model string) *Session {
	now := time.Now()
	return &Session{ID: now.Format("20060102-150405-000"), Modelo: model,
		Criada: now.Format(time.RFC3339), Atualizada: now.Format(time.RFC3339)}
}

func oneLine(s string) string { return strings.Join(strings.Fields(s), " ") }

func truncRunes(s string, n int) string {
	if utf8.RuneCountInString(s) <= n {
		return s
	}
	r := []rune(s)
	return string(r[:n]) + "..."
}

// grava a sessao atual conforme o evento; chamar com a.mu travado
func (a *App) record(e Event) {
	s := a.sess
	if s == nil {
		return
	}
	switch e.T {
	case "user":
		s.Mensagens = append(s.Mensagens, Msg{Papel: "user", Texto: e.Text})
		if s.Titulo == "" {
			s.Titulo = truncRunes(oneLine(e.Text), 80)
		}
		a.dirty = true
		a.saveLocked()
	case "text":
		if n := len(s.Mensagens); n > 0 && s.Mensagens[n-1].Papel == "assistant" {
			s.Mensagens[n-1].Texto += e.Text
		} else if strings.TrimSpace(e.Text) != "" {
			s.Mensagens = append(s.Mensagens, Msg{Papel: "assistant", Texto: strings.TrimLeft(e.Text, " \t\r\n")})
		} else {
			return
		}
		a.dirty = true
	case "tool":
		s.Mensagens = append(s.Mensagens, Msg{Papel: "tool", Nome: e.Name, Texto: e.Text})
		a.dirty = true
	case "ready":
		if a.dirty {
			a.saveLocked()
		}
	}
}

func (a *App) saveLocked() {
	s := a.sess
	if s == nil {
		return
	}
	s.Atualizada = time.Now().Format(time.RFC3339)
	if a.model != "" {
		s.Modelo = a.model
	}
	if saveSession(a.sessDir, s) == nil {
		a.dirty = false
	}
}

// escrita atomica: arquivo temporario + rename
func saveSession(dir string, s *Session) error {
	if !validID(s.ID) {
		return os.ErrInvalid
	}
	b, err := json.MarshalIndent(s, "", "  ")
	if err != nil {
		return err
	}
	os.MkdirAll(dir, 0o755)
	tmp, err := os.CreateTemp(dir, ".tmp-*")
	if err != nil {
		return err
	}
	_, err = tmp.Write(b)
	if cerr := tmp.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		os.Remove(tmp.Name())
		return err
	}
	if err := os.Rename(tmp.Name(), filepath.Join(dir, s.ID+".json")); err != nil {
		os.Remove(tmp.Name())
		return err
	}
	return nil
}

func loadSession(dir, id string) (*Session, error) {
	if !validID(id) {
		return nil, os.ErrInvalid
	}
	b, err := os.ReadFile(filepath.Join(dir, id+".json"))
	if err != nil {
		return nil, err
	}
	var s Session
	if err := json.Unmarshal(b, &s); err != nil {
		return nil, err
	}
	s.ID = id
	return &s, nil
}

func listSessions(dir, cur string) []SessionInfo {
	out := []SessionInfo{}
	files, _ := filepath.Glob(filepath.Join(dir, "*.json"))
	for _, f := range files {
		id := strings.TrimSuffix(filepath.Base(f), ".json")
		s, err := loadSession(dir, id)
		if err != nil {
			continue
		}
		out = append(out, SessionInfo{ID: id, Titulo: s.Titulo, Modelo: s.Modelo, Criada: s.Criada,
			Atualizada: s.Atualizada, Mensagens: len(s.Mensagens), Atual: id == cur})
	}
	sort.Slice(out, func(i, j int) bool {
		if out[i].Atualizada != out[j].Atualizada {
			return out[i].Atualizada > out[j].Atualizada
		}
		return out[i].ID > out[j].ID
	})
	return out
}

// resumo textual da conversa anterior (so usuario e assistente), limitado mantendo o fim
func buildContext(ms []Msg) string {
	var b strings.Builder
	for _, m := range ms {
		switch m.Papel {
		case "user":
			b.WriteString("Voce: " + oneLine(m.Texto) + "\n")
		case "assistant":
			b.WriteString("Assistente: " + oneLine(m.Texto) + "\n")
		}
	}
	s := b.String()
	if s == "" {
		return ""
	}
	if r := []rune(s); len(r) > maxContexto {
		s = "..." + string(r[len(r)-maxContexto:])
	}
	return "Contexto da conversa anterior:\n" + s + "\n"
}
