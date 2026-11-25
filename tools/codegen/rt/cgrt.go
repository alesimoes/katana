// ---- runtime do backend Go do UCGE (docs/ucge.md, "Backend Go"): erros, inteiros, strings, listas, mapas e valores etiquetados ----
// Este arquivo e a fonte; src/coder/ucgego_rt.inc e gerado dele por tools/ucge/rt2inc.py (o gerador o copia para o arquivo de saida).

// CgErr e o valor de panic de um `throw`; try/catch o recupera.
type CgErr struct{ Msg string }

func cgThrow(msg string) { panic(CgErr{msg}) }
func cgMsg(r any) string {
	switch e := r.(type) {
	case CgErr:
		return e.Msg
	case error:
		return e.Error()
	case string:
		return e
	}
	return fmt.Sprint(r)
}

// cgProtect roda body e devolve o codigo de controle dele, ou o erro recuperado.
func cgProtect(body func() int) (ctl int, caught bool, msg string) {
	defer func() {
		if r := recover(); r != nil {
			caught, msg = true, cgMsg(r)
		}
	}()
	return body(), false, ""
}

// cgTry: try/catch/finally. Os codigos de controle (0 normal, 1 break, 2 continue, 3 return) voltam para o chamador.
func cgTry(body func() int, catch func(string) int, fin func()) int {
	if fin != nil {
		defer fin()
	}
	if catch == nil {
		return body()
	}
	ctl, caught, msg := cgProtect(body)
	if caught {
		return catch(msg)
	}
	return ctl
}

// inteiros de 64 bits: estouro vira erro (o Python tem inteiros ilimitados); // e % arredondam para baixo
func cgAdd(a, b int64) int64 {
	c := a + b
	if (c > a) != (b > 0) {
		cgThrow("integer overflow")
	}
	return c
}
func cgSub(a, b int64) int64 {
	c := a - b
	if (c < a) != (b > 0) {
		cgThrow("integer overflow")
	}
	return c
}
func cgMul(a, b int64) int64 {
	if a == 0 || b == 0 {
		return 0
	}
	c := a * b
	if (c < 0) != ((a < 0) != (b < 0)) || c/b != a || (a == -1 && b == math.MinInt64) || (b == -1 && a == math.MinInt64) {
		cgThrow("integer overflow")
	}
	return c
}
func cgNeg(a int64) int64 {
	if a == math.MinInt64 {
		cgThrow("integer overflow")
	}
	return -a
}
func cgIDiv(a, b int64) int64 {
	if b == 0 {
		cgThrow("integer division or modulo by zero")
	}
	if a == math.MinInt64 && b == -1 {
		cgThrow("integer overflow")
	}
	q := a / b
	if a%b != 0 && (a < 0) != (b < 0) {
		q--
	}
	return q
}
func cgMod(a, b int64) int64 {
	if b == 0 {
		cgThrow("integer modulo by zero")
	}
	if b == -1 {
		return 0
	}
	r := a % b
	if r != 0 && (r < 0) != (b < 0) {
		r += b
	}
	return r
}

// aritmetica de float e concatenacao como funcoes: o gofmt aperta os operadores `+ - *` conforme a profundidade da expressao, entao o gerador so os
// escreve nativos quando a expressao inteira esta no topo de uma instrucao
func cgFAdd(a, b float64) float64 { return a + b }
func cgFSub(a, b float64) float64 { return a - b }
func cgFMul(a, b float64) float64 { return a * b }
func cgCat(a, b string) string    { return a + b }
func cgFDiv(a, b float64) float64 {
	if b == 0 {
		cgThrow("division by zero")
	}
	return a / b
}
func cgFMod(a, b float64) float64 {
	if b == 0 {
		cgThrow("float modulo")
	}
	r := math.Mod(a, b)
	if r != 0 && (r < 0) != (b < 0) {
		r += b
	}
	return r
}
func cgAbs(a int64) int64 {
	if a == math.MinInt64 {
		cgThrow("integer overflow")
	}
	if a < 0 {
		return -a
	}
	return a
}
func cgSqrt(x float64) float64 {
	if x < 0 {
		cgThrow("math domain error")
	}
	return math.Sqrt(x)
}
func cgPow(a, b float64) float64 {
	r := math.Pow(a, b)
	if math.IsNaN(r) && !math.IsNaN(a) && !math.IsNaN(b) {
		cgThrow("math domain error")
	}
	if math.IsInf(r, 0) && !math.IsInf(a, 0) && !math.IsInf(b, 0) {
		cgThrow("math range error")
	}
	return r
}
func cgF2I(d float64) int64 {
	if math.IsNaN(d) {
		cgThrow("cannot convert float NaN to integer")
	}
	if math.IsInf(d, 0) {
		cgThrow("cannot convert float infinity to integer")
	}
	if d >= 9223372036854775808.0 || d < -9223372036854775808.0 {
		cgThrow("integer overflow")
	}
	return int64(d)
}
func cgFloor(d float64) int64 { return cgF2I(math.Floor(d)) }
func cgIMin(a, b int64) int64 {
	if b < a {
		return b
	}
	return a
}
func cgIMax(a, b int64) int64 {
	if b > a {
		return b
	}
	return a
}
func cgFMin(a, b float64) float64 {
	if b < a {
		return b
	}
	return a
}
func cgFMax(a, b float64) float64 {
	if b > a {
		return b
	}
	return a
}

// o indice de uma lista ou string aceita negativos como no Python
func cgIx(i int64, n int, what string) int {
	if i < 0 {
		i += int64(n)
	}
	if i < 0 || i >= int64(n) {
		cgThrow(what + " index out of range")
	}
	return int(i)
}

// texto: o formato dos floats e o do repr do Python (o menor que volta ao mesmo valor)
func cgFStr(d float64) string {
	if math.IsNaN(d) {
		return "nan"
	}
	if math.IsInf(d, 1) {
		return "inf"
	}
	if math.IsInf(d, -1) {
		return "-inf"
	}
	t := strconv.FormatFloat(d, 'e', -1, 64)
	neg := ""
	if t[0] == '-' {
		neg, t = "-", t[1:]
	}
	ep := strings.IndexByte(t, 'e')
	e10, _ := strconv.Atoi(t[ep+1:])
	dg := strings.Replace(t[:ep], ".", "", 1)
	dg = strings.TrimRight(dg, "0")
	if dg == "" {
		return neg + "0.0"
	}
	if e10 >= -4 && e10 < 16 {
		dp := e10 + 1
		switch {
		case dp <= 0:
			return neg + "0." + strings.Repeat("0", -dp) + dg
		case dp >= len(dg):
			return neg + dg + strings.Repeat("0", dp-len(dg)) + ".0"
		}
		return neg + dg[:dp] + "." + dg[dp:]
	}
	m := dg[:1]
	if len(dg) > 1 {
		m += "." + dg[1:]
	}
	sign := "+"
	if e10 < 0 {
		sign, e10 = "-", -e10
	}
	return fmt.Sprintf("%s%se%s%02d", neg, m, sign, e10)
}
func cgBStr(b bool) string {
	if b {
		return "True"
	}
	return "False"
}
func cgS2I(s string) int64 {
	o := s
	s = strings.TrimFunc(s, cgIsWS)
	t := strings.ReplaceAll(s, "_", "")
	if strings.Contains(s, "__") || strings.HasPrefix(strings.TrimLeft(s, "+-"), "_") || strings.HasSuffix(s, "_") {
		cgThrow("invalid literal for int() with base 10: " + cgSRep(o))
	}
	v, err := strconv.ParseInt(t, 10, 64)
	if err != nil {
		if ne, ok := err.(*strconv.NumError); ok && ne.Err == strconv.ErrRange {
			cgThrow("integer overflow")
		}
		cgThrow("invalid literal for int() with base 10: " + cgSRep(o))
	}
	return v
}
func cgS2F(s string) float64 {
	s = strings.TrimFunc(s, cgIsWS)
	v, err := strconv.ParseFloat(s, 64)
	if err != nil {
		if ne, ok := err.(*strconv.NumError); ok && ne.Err == strconv.ErrRange {
			return v
		}
		cgThrow("could not convert string to float")
	}
	return v
}
func cgIsWS(r rune) bool { return r == ' ' || (r >= 9 && r <= 13) || (r >= 28 && r <= 31) }
func cgSRep(s string) string {
	q := byte('\'')
	if strings.Contains(s, "'") && !strings.Contains(s, "\"") {
		q = '"'
	}
	var b strings.Builder
	b.WriteByte(q)
	for i := 0; i < len(s); i++ {
		c := s[i]
		switch {
		case c == q || c == '\\':
			b.WriteByte('\\')
			b.WriteByte(c)
		case c == '\n':
			b.WriteString("\\n")
		case c == '\t':
			b.WriteString("\\t")
		case c == '\r':
			b.WriteString("\\r")
		case c < 32 || c == 127:
			fmt.Fprintf(&b, "\\x%02x", c)
		default:
			b.WriteByte(c)
		}
	}
	b.WriteByte(q)
	return b.String()
}

// fatias do Python: negativos contam do fim, o resto e limitado
func cgSliceB(n int, a, b int64) (int, int) {
	x, y := a, b
	if x < 0 {
		x += int64(n)
		if x < 0 {
			x = 0
		}
	} else if x > int64(n) {
		x = int64(n)
	}
	if y < 0 {
		y += int64(n)
		if y < 0 {
			y = 0
		}
	} else if y > int64(n) {
		y = int64(n)
	}
	if y < x {
		y = x
	}
	return int(x), int(y)
}
func cgSSlice(s string, a, b int64) string { x, y := cgSliceB(len(s), a, b); return s[x:y] }
func cgSIdx(s string, i int64) string      { k := cgIx(i, len(s), "string"); return s[k : k+1] }
func cgSFind(s, sub string) int64          { return int64(strings.Index(s, sub)) }
func cgSUpper(s string) string {
	b := []byte(s)
	for i, c := range b {
		if c >= 'a' && c <= 'z' {
			b[i] = c - 32
		}
	}
	return string(b)
}
func cgSLower(s string) string {
	b := []byte(s)
	for i, c := range b {
		if c >= 'A' && c <= 'Z' {
			b[i] = c + 32
		}
	}
	return string(b)
}
func cgSTrim(s string) string { return strings.TrimFunc(s, cgIsWS) }
func cgSRev(s string) string {
	b := []byte(s)
	for i, j := 0, len(b)-1; i < j; i, j = i+1, j-1 {
		b[i], b[j] = b[j], b[i]
	}
	return string(b)
}
func cgSReplace(s, o, w string) string { return strings.ReplaceAll(s, o, w) }
func cgSSplit(s, sep string) *List[string] {
	if sep == "" {
		cgThrow("empty separator")
	}
	return &List[string]{strings.Split(s, sep)}
}
func cgSWords(s string) *List[string] { return &List[string]{strings.FieldsFunc(s, cgIsWS)} }

var cgReCache = map[string]*regexp.Regexp{}

func cgMatches(s, re string) bool {
	r, ok := cgReCache[re]
	if !ok {
		var err error
		r, err = regexp.Compile(re)
		if err != nil {
			cgThrow("regex: " + err.Error())
		}
		cgReCache[re] = r
	}
	return r.MatchString(s)
}

// listas: List[T] guarda o vetor; Lister e a vista dinamica (o tipo estatico list[any])
type List[T any] struct{ a []T }
type Lister interface {
	Len() int
	AnyAt(i int) any
	SetAny(i int, v any)
	PushAny(v any)
	PopAny() any
	SliceAny(a, b int64) Lister
	RevAny() Lister
	NewLike() Lister
	Raw() any
}

func cgNewList[T any](xs ...T) *List[T] { return &List[T]{append([]T(nil), xs...)} }
func (l *List[T]) nn() *List[T] {
	if l == nil {
		cgThrow("null list")
	}
	return l
}
func (l *List[T]) Len() int         { return len(l.nn().a) }
func (l *List[T]) Get(i int64) T    { l.nn(); return l.a[cgIx(i, len(l.a), "list")] }
func (l *List[T]) Set(i int64, v T) { l.nn(); l.a[cgIx(i, len(l.a), "list")] = v }
func (l *List[T]) Push(v T)         { l.nn(); l.a = append(l.a, v) }
func (l *List[T]) Pop() T {
	l.nn()
	if len(l.a) == 0 {
		cgThrow("pop from empty list")
	}
	v := l.a[len(l.a)-1]
	l.a = l.a[:len(l.a)-1]
	return v
}
func (l *List[T]) AnyAt(i int) any     { return l.a[i] }
func (l *List[T]) SetAny(i int, v any) { l.nn(); l.a[cgIx(int64(i), len(l.a), "list")] = cgAs[T](v) }
func (l *List[T]) PushAny(v any)       { l.Push(cgAs[T](v)) }
func (l *List[T]) PopAny() any         { return l.Pop() }
func (l *List[T]) SliceAny(a, b int64) Lister {
	x, y := cgSliceB(len(l.nn().a), a, b)
	return &List[T]{append([]T(nil), l.a[x:y]...)}
}
func (l *List[T]) RevAny() Lister {
	r := make([]T, len(l.nn().a))
	for i, v := range l.a {
		r[len(r)-1-i] = v
	}
	return &List[T]{r}
}
func (l *List[T]) NewLike() Lister { return &List[T]{} }
func (l *List[T]) Raw() any        { return l }

// cgAs: um valor etiquetado de volta ao tipo estatico (int vira float quando o destino e float)
func cgAs[T any](v any) T {
	if x, ok := v.(T); ok {
		return x
	}
	var z T
	if v == nil {
		return z
	}
	if p, ok := any(&z).(*float64); ok {
		if i, ok := v.(int64); ok {
			*p = float64(i)
			return z
		}
	}
	cgThrow(fmt.Sprintf("type error: expected %T, found %s", z, cgTName(v)))
	return z
}
func cgTName(v any) string {
	switch v.(type) {
	case nil:
		return "None"
	case bool:
		return "bool"
	case int64:
		return "int"
	case float64:
		return "float"
	case string:
		return "str"
	case Lister:
		return "list"
	case Mapper:
		return "dict"
	}
	return "object"
}

// cgList: any -> lista tipada (uma lista dinamica vazia vira uma lista vazia do tipo pedido; senao os elementos sao convertidos)
func cgList[T any](v any) *List[T] {
	if v == nil {
		return nil
	}
	if l, ok := v.(*List[T]); ok {
		return l
	}
	d, ok := v.(Lister)
	if !ok {
		cgThrow("type error: expected list, found " + cgTName(v))
	}
	r := &List[T]{}
	for i := 0; i < d.Len(); i++ {
		r.a = append(r.a, cgAs[T](d.AnyAt(i)))
	}
	return r
}

// cgUL e cgUM: any -> lista ou mapa dinamico (nil continua nulo)
func cgUL(v any) Lister {
	if v == nil {
		return nil
	}
	return cgLister(v)
}
func cgUM(v any) Mapper {
	if v == nil {
		return nil
	}
	return cgMapper(v)
}
func cgLister(l any) Lister {
	if l == nil {
		cgThrow("null list")
	}
	d, ok := l.(Lister)
	if !ok {
		cgThrow("type error: expected list, found " + cgTName(l))
	}
	return d
}

// mapas str -> T na ordem de insercao
type Map[T any] struct {
	keys []string
	idx  map[string]int
	vals []T
}
type Mapper interface {
	Len() int
	HasKey(k string) bool
	AnyGet(k string) any
	AnySet(k string, v any)
	KeyAt(i int) string
	AnyValAt(i int) any
}

func cgNewMap[T any]() *Map[T] { return &Map[T]{idx: map[string]int{}} }

// cgMapLit: o literal {"k": v, ...}; os pares chegam como any e os valores voltam ao tipo do mapa
func cgMapLit[T any](kv ...any) *Map[T] {
	m := cgNewMap[T]()
	for i := 0; i+1 < len(kv); i += 2 {
		m.Set(kv[i].(string), cgAs[T](kv[i+1]))
	}
	return m
}
func (m *Map[T]) nn() *Map[T] {
	if m == nil {
		cgThrow("null map")
	}
	return m
}
func (m *Map[T]) Len() int             { return len(m.nn().keys) }
func (m *Map[T]) HasKey(k string) bool { _, ok := m.nn().idx[k]; return ok }
func (m *Map[T]) Get(k string) T {
	i, ok := m.nn().idx[k]
	if !ok {
		cgThrow("'" + k + "'")
	}
	return m.vals[i]
}
func (m *Map[T]) Set(k string, v T) {
	if i, ok := m.nn().idx[k]; ok {
		m.vals[i] = v
		return
	}
	m.idx[k] = len(m.keys)
	m.keys = append(m.keys, k)
	m.vals = append(m.vals, v)
}
func (m *Map[T]) AnyGet(k string) any    { return m.Get(k) }
func (m *Map[T]) AnySet(k string, v any) { m.Set(k, cgAs[T](v)) }
func (m *Map[T]) KeyAt(i int) string     { return m.keys[i] }
func (m *Map[T]) AnyValAt(i int) any     { return m.vals[i] }
func cgMap[T any](v any) *Map[T] {
	if v == nil {
		return nil
	}
	if m, ok := v.(*Map[T]); ok {
		return m
	}
	d, ok := v.(Mapper)
	if !ok {
		cgThrow("type error: expected dict, found " + cgTName(v))
	}
	r := cgNewMap[T]()
	for i := 0; i < d.Len(); i++ {
		r.Set(d.KeyAt(i), cgAs[T](d.AnyValAt(i)))
	}
	return r
}
func cgMapper(m any) Mapper {
	if m == nil {
		cgThrow("null map")
	}
	d, ok := m.(Mapper)
	if !ok {
		cgThrow("type error: expected dict, found " + cgTName(m))
	}
	return d
}
func cgKeys(m Mapper) *List[string] {
	if m == nil {
		cgThrow("null map")
	}
	r := &List[string]{}
	for i := 0; i < m.Len(); i++ {
		r.a = append(r.a, m.KeyAt(i))
	}
	return r
}

// valores etiquetados (any): igualdade, ordem e texto
func cgIsNum(v any) bool {
	switch v.(type) {
	case bool, int64, float64:
		return true
	}
	return false
}
func cgNumF(v any) float64 {
	switch x := v.(type) {
	case bool:
		if x {
			return 1
		}
		return 0
	case int64:
		return float64(x)
	}
	return v.(float64)
}

// cgBox: ponteiro de lista, mapa ou objeto para any; o ponteiro nulo vira nil (senao o any nao seria nulo)
func cgBox[P any](p *P) any {
	if p == nil {
		return nil
	}
	return p
}

func cgEq(a, b any) bool {
	if a == nil || b == nil {
		return a == nil && b == nil
	}
	if cgIsNum(a) && cgIsNum(b) {
		ai, aok := a.(int64)
		bi, bok := b.(int64)
		if aok && bok {
			return ai == bi
		}
		return cgNumF(a) == cgNumF(b)
	}
	switch x := a.(type) {
	case string:
		y, ok := b.(string)
		return ok && x == y
	case Lister:
		y, ok := b.(Lister)
		if !ok || x.Len() != y.Len() {
			return false
		}
		for i := 0; i < x.Len(); i++ {
			if !cgEq(x.AnyAt(i), y.AnyAt(i)) {
				return false
			}
		}
		return true
	case Mapper:
		y, ok := b.(Mapper)
		if !ok || x.Len() != y.Len() {
			return false
		}
		for i := 0; i < x.Len(); i++ {
			if !y.HasKey(x.KeyAt(i)) || !cgEq(x.AnyValAt(i), y.AnyGet(x.KeyAt(i))) {
				return false
			}
		}
		return true
	}
	return a == b
}
func cgCmp(a, b any) int {
	if cgIsNum(a) && cgIsNum(b) {
		ai, aok := a.(int64)
		bi, bok := b.(int64)
		if aok && bok {
			switch {
			case ai < bi:
				return -1
			case ai > bi:
				return 1
			}
			return 0
		}
		x, y := cgNumF(a), cgNumF(b)
		switch {
		case x < y:
			return -1
		case x > y:
			return 1
		}
		return 0
	}
	x, ok1 := a.(string)
	y, ok2 := b.(string)
	if ok1 && ok2 {
		return strings.Compare(x, y)
	}
	cgThrow("type error: cannot order " + cgTName(a) + " and " + cgTName(b))
	return 0
}
func cgFmt(b *strings.Builder, v any, rep bool) {
	switch x := v.(type) {
	case nil:
		b.WriteString("None")
	case bool:
		b.WriteString(cgBStr(x))
	case int64:
		b.WriteString(strconv.FormatInt(x, 10))
	case float64:
		b.WriteString(cgFStr(x))
	case string:
		if rep {
			b.WriteString(cgSRep(x))
		} else {
			b.WriteString(x)
		}
	case Lister:
		b.WriteByte('[')
		for i := 0; i < x.Len(); i++ {
			if i > 0 {
				b.WriteString(", ")
			}
			cgFmt(b, x.AnyAt(i), true)
		}
		b.WriteByte(']')
	case Mapper:
		b.WriteByte('{')
		for i := 0; i < x.Len(); i++ {
			if i > 0 {
				b.WriteString(", ")
			}
			b.WriteString(cgSRep(x.KeyAt(i)))
			b.WriteString(": ")
			cgFmt(b, x.AnyValAt(i), true)
		}
		b.WriteByte('}')
	default:
		b.WriteString("<object>")
	}
}
func cgStr(v any) string {
	if s, ok := v.(string); ok {
		return s
	}
	var b strings.Builder
	cgFmt(&b, v, false)
	return b.String()
}
func cgPrint(v any) { fmt.Println(cgStr(v)) }

// operacoes dinamicas (operandos any): so existem quando o tipo nao foi dado; o erro de tipo e em tempo de execucao
func cgArith(op int, a, b any) any {
	ai, aok := a.(int64)
	bi, bok := b.(int64)
	if ab, ok := a.(bool); ok {
		ai, aok = 0, true
		if ab {
			ai = 1
		}
	}
	if bb, ok := b.(bool); ok {
		bi, bok = 0, true
		if bb {
			bi = 1
		}
	}
	if aok && bok {
		switch op {
		case 0:
			return cgAdd(ai, bi)
		case 1:
			return cgSub(ai, bi)
		case 2:
			return cgMul(ai, bi)
		case 3:
			return cgFDiv(float64(ai), float64(bi))
		case 4:
			return cgIDiv(ai, bi)
		}
		return cgMod(ai, bi)
	}
	if cgIsNum(a) && cgIsNum(b) {
		x, y := cgNumF(a), cgNumF(b)
		switch op {
		case 0:
			return x + y
		case 1:
			return x - y
		case 2:
			return x * y
		case 3:
			return cgFDiv(x, y)
		case 4:
			return math.Floor(cgFDiv(x, y))
		}
		return cgFMod(x, y)
	}
	if op == 0 {
		if x, ok := a.(string); ok {
			if y, ok := b.(string); ok {
				return x + y
			}
		}
		if x, ok := a.(Lister); ok {
			if y, ok := b.(Lister); ok {
				r := &List[any]{}
				for i := 0; i < x.Len(); i++ {
					r.a = append(r.a, x.AnyAt(i))
				}
				for i := 0; i < y.Len(); i++ {
					r.a = append(r.a, y.AnyAt(i))
				}
				return r
			}
		}
	}
	cgThrow("type error: unsupported operand types " + cgTName(a) + " and " + cgTName(b))
	return nil
}
func cgVNeg(a any) any {
	switch x := a.(type) {
	case int64:
		return cgNeg(x)
	case float64:
		return -x
	}
	cgThrow("type error: expected number, found " + cgTName(a))
	return nil
}
func cgVGet(c, i any) any {
	switch x := c.(type) {
	case Lister:
		return x.AnyAt(cgIx(cgAs[int64](i), x.Len(), "list"))
	case Mapper:
		return x.AnyGet(cgAs[string](i))
	case string:
		return cgSIdx(x, cgAs[int64](i))
	}
	cgThrow("type error: expected list, map or str, found " + cgTName(c))
	return nil
}
func cgVSet(c, i, v any) {
	switch x := c.(type) {
	case Lister:
		x.SetAny(int(cgAs[int64](i)), v)
		return
	case Mapper:
		x.AnySet(cgAs[string](i), v)
		return
	}
	cgThrow("type error: expected list or map, found " + cgTName(c))
}
func cgVLen(c any) int64 {
	switch x := c.(type) {
	case Lister:
		return int64(x.Len())
	case Mapper:
		return int64(x.Len())
	case string:
		return int64(len(x))
	}
	cgThrow("type error: expected list, map or str, found " + cgTName(c))
	return 0
}
func cgVHas(c, x any) bool {
	switch d := c.(type) {
	case Lister:
		for i := 0; i < d.Len(); i++ {
			if cgEq(d.AnyAt(i), x) {
				return true
			}
		}
		return false
	case string:
		return strings.Contains(d, cgAs[string](x))
	case Mapper:
		return d.HasKey(cgAs[string](x))
	}
	cgThrow("type error: expected list, map or str, found " + cgTName(c))
	return false
}
func cgV2I(v any) int64 {
	switch x := v.(type) {
	case int64:
		return x
	case bool:
		if x {
			return 1
		}
		return 0
	case float64:
		return cgF2I(x)
	case string:
		return cgS2I(x)
	}
	cgThrow("type error: expected number or str, found " + cgTName(v))
	return 0
}
func cgV2F(v any) float64 {
	if s, ok := v.(string); ok {
		return cgS2F(s)
	}
	if !cgIsNum(v) {
		cgThrow("type error: expected number or str, found " + cgTName(v))
	}
	return cgNumF(v)
}
func cgVSlice(c any, a, b int64) any {
	switch x := c.(type) {
	case string:
		return cgSSlice(x, a, b)
	case Lister:
		return x.SliceAny(a, b)
	}
	cgThrow("type error: expected list or str, found " + cgTName(c))
	return nil
}
func cgVRev(c any) any {
	switch x := c.(type) {
	case string:
		return cgSRev(x)
	case Lister:
		return x.RevAny()
	}
	cgThrow("type error: expected list or str, found " + cgTName(c))
	return nil
}
func cgVAbs(v any) any {
	switch x := v.(type) {
	case int64:
		return cgAbs(x)
	case float64:
		return math.Abs(x)
	}
	cgThrow("type error: expected number, found " + cgTName(v))
	return nil
}
func cgVMin(a, b any) any {
	if cgCmp(b, a) < 0 {
		return b
	}
	return a
}
func cgVMax(a, b any) any {
	if cgCmp(b, a) > 0 {
		return b
	}
	return a
}
func cgSort(l Lister) Lister {
	r := l.NewLike()
	n := l.Len()
	v := make([]any, n)
	for i := range v {
		v[i] = l.AnyAt(i)
	}
	sort.SliceStable(v, func(i, j int) bool { return cgCmp(v[i], v[j]) < 0 })
	for _, x := range v {
		r.PushAny(x)
	}
	return r
}
func cgSum(l Lister) any {
	var acc any = int64(0)
	for i := 0; i < l.Len(); i++ {
		acc = cgArith(0, acc, l.AnyAt(i))
	}
	return acc
}
func cgJoin(l Lister, sep string) string {
	p := make([]string, l.Len())
	for i := range p {
		s, ok := l.AnyAt(i).(string)
		if !ok {
			cgThrow("type error: expected str, found " + cgTName(l.AnyAt(i)))
		}
		p[i] = s
	}
	return strings.Join(p, sep)
}
func cgItem(c any, i int) any { // for x in c: item i (lista: elemento; str: caractere; mapa: chave)
	switch x := c.(type) {
	case Lister:
		return x.AnyAt(i)
	case string:
		return x[i : i+1]
	case Mapper:
		return x.KeyAt(i)
	}
	cgThrow("type error: expected list, map or str, found " + cgTName(c))
	return nil
}
func cgItemV(c any, i int) any { return cgMapper(c).AnyValAt(i) }

func cgTestFail(n int, got, exp any) {
	var b strings.Builder
	cgFmt(&b, got, true)
	g := b.String()
	b.Reset()
	cgFmt(&b, exp, true)
	fmt.Printf("test %d: FAIL got %s expected %s\n", n, g, b.String())
}
func cgTestRun(n int, f func(), raises bool) {
	_, caught, msg := cgProtect(func() int { f(); return 0 })
	switch {
	case caught && raises:
		fmt.Printf("test %d: ok\n", n)
	case caught:
		fmt.Printf("test %d: FAIL raised %s\n", n, msg)
	case raises:
		fmt.Printf("test %d: FAIL did not raise\n", n)
	}
}

var _ = os.Exit
