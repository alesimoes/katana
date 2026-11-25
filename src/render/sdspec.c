/* ============================ render: pedido em linguagem natural -> especificacao de imagem -> prompt do modelo de imagem ===========
 * katana render spec "<pedido>" [--ref preview.png] [--lcm] [--chunks 4] [--long 512] [--final [--final-engine flux|krea]] [--json] [--no-rag]
 * katana render generate --nl "<pedido>" --out saida.png [--ref ref.png] ...                     gera a partir do pedido inteiro
 *
 * O motor entende o pedido sozinho, sem nenhum modelo de linguagem no caminho:
 *   1. Analise lexica   normaliza (minusculas, sem acento), separa em clausulas, acha os conceitos por palavra (estilo, luz, composicao,
 *                       camera, proporcao, finalidade), os pedidos negativos ("sem X") e os sinais de importancia ("principal", "destaque",
 *                       "ao fundo"); o conteudo passa por um lexico PT->EN.
 *   2. Embedding        a clausula que nenhuma palavra reconhece e comparada (EmbeddingGemma, multilingue) com exemplos naturais em PT e EN
 *                       de cada conceito; entra o conceito do melhor exemplo se passa do limiar e tem folga sobre o melhor conceito diferente.
 *   3. Roteador         classifica o dominio (arquitetura exterior/interior, produto, pessoa, paisagem) e o compilador de cena preenche so as
 *                       lacunas que o pedido deixou (enquadramento, luz, camera, negativos, proporcao).
 *   4. Prioridade       assunto primeiro, depois o que o pedido enfatiza; o que nao cabe nos blocos de 75 tokens do CLIP cai do fim.
 *   5. Configuracao     codigo: proporcao -> largura x altura (multiplos de 64), passos, guidance, forca da referencia. O usuario nao pede passos.
 *   6. Adaptador        SdSpec -> prompt do Stable (blocos de 75 tokens, pesos (x:1.2)); o final (FLUX/Krea) recebe prosa e o preview como referencia.
 * "remova/troque o fundo" (inpaint) e reconhecido e avisado; sem mascara, segue como imagem a imagem. */

typedef struct {
    char intent[16];                                    /* text_to_image | image_to_image | inpaint (so reconhecido) */
    char subject[240], environment[240], style[200], lighting[200], composition[200], camera[160], quality[200], extra[240], negative[300];
    char domain[24], scene[32];                          /* roteador: architecture, product, person, landscape, general; cena (exterior/interior) */
    char purpose[24];                                   /* wallpaper, profile, banner, ... ou vazio */
    int final_mode;                                     /* 0 = preview (composicao forte, rapido), 1 = final (mais passos e detalhe) */
    double aspect;                                      /* largura/altura pedida; 0 = nao pedida */
    char request[1200];                                 /* o pedido inteiro (consulta do embedding na priorizacao) */
    int rag;                                            /* o embedding pode ser usado */
    int person;                                         /* o assunto tem uma pessoa (negativos de anatomia) */
    char unknown[200];                                  /* palavras do conteudo sem traducao */
} SdSpec;

enum { SC_STYLE, SC_LIGHT, SC_COMP, SC_CAMERA, SC_QUALITY, SC_NEG, SC_ASPECT, SC_PURPOSE };

typedef struct { int cat; const char *keys; const char *phrase; double aspect; const char *purpose; } SdsConcept;

/* keys: radicais PT e EN sem acento, separados por '|', casam no inicio de palavra (radicais de ate 3 letras tambem no fim) */
static const SdsConcept SDS_C[] = {
    { SC_STYLE, "fotorreal|fotografia|foto |photo|photograph|realist|realism", "professional photograph, photorealistic", 0, "" },
    { SC_STYLE, "3d |cgi|octane|modelagem 3d", "3d render, octane render", 0, "" },
    { SC_STYLE, "pixar|disney|animacao 3d|animated", "3d animated movie style, pixar style", 0, "" },
    { SC_STYLE, "anime|manga", "anime style, cel shading, detailed illustration", 0, "" },
    { SC_STYLE, "aquarela|watercolor", "watercolor painting, soft washes, paper texture", 0, "" },
    { SC_STYLE, "pintura a oleo|oil painting", "oil painting, visible brush strokes", 0, "" },
    { SC_STYLE, "pintura|painting", "digital painting, concept art", 0, "" },
    { SC_STYLE, "ilustra|illustration", "detailed illustration", 0, "" },
    { SC_STYLE, "desenho a lapis|lapis|pencil", "pencil drawing, graphite, hatching", 0, "" },
    { SC_STYLE, "cinematogr|cinematic|filme|movie still", "cinematic film still, anamorphic, film grain", 0, "" },
    { SC_STYLE, "cyberpunk", "cyberpunk, neon, futuristic city", 0, "" },
    { SC_STYLE, "futurist|futuristic|sci-fi|ficcao cientifica", "futuristic, sci-fi design", 0, "" },
    { SC_STYLE, "minimalis|minimalist", "minimalist, clean, uncluttered", 0, "" },
    { SC_STYLE, "vintage|retro", "vintage, retro look", 0, "" },
    { SC_STYLE, "arquitet|architectur|interior", "architectural interior photograph", 0, "" },
    { SC_STYLE, "moderno|modern|contemporane", "modern contemporary design", 0, "" },
    { SC_STYLE, "luxo|luxur|sofisticad|elegante|elegant", "luxurious, elegant, refined", 0, "" },
    { SC_LIGHT, "cinematograf iluminacao|iluminacao cinematogr|cinematic lighting", "cinematic lighting, dramatic contrast", 0, "" },
    { SC_LIGHT, "luz natural|natural light|luz do dia|daylight|luz do sol|sunlight", "soft natural daylight", 0, "" },
    { SC_LIGHT, "estudio|studio", "professional studio lighting, softbox", 0, "" },
    { SC_LIGHT, "neon", "neon lighting, glowing colors", 0, "" },
    { SC_LIGHT, "noite|noturn|night", "night scene, dark sky, artificial lights", 0, "" },
    { SC_LIGHT, "por do sol|entardecer|golden hour|hora dourada|sunset", "golden hour, warm low sun", 0, "" },
    { SC_LIGHT, "amanhecer|nascer do sol|sunrise", "sunrise, soft warm light", 0, "" },
    { SC_LIGHT, "luz suave|soft light|difusa|diffused", "soft diffused light", 0, "" },
    { SC_LIGHT, "luz quente|warm light|aconchegante|cozy", "warm cozy lighting, 3000K", 0, "" },
    { SC_LIGHT, "dramatic|dramatic|contraluz|backlit", "dramatic lighting, rim light", 0, "" },
    { SC_LIGHT, "nublado|overcast|neblina|fog", "overcast diffused light, soft mist", 0, "" },
    { SC_COMP, "close-up|close up|primeiro plano|plano detalhe", "close-up shot", 0, "" },
    { SC_COMP, "plano medio|medium shot", "medium shot", 0, "" },
    { SC_COMP, "plano aberto|wide shot|grande angular|wide angle|panoramic", "wide shot, wide angle", 0, "" },
    { SC_COMP, "vista aerea|aerial|drone|de cima", "aerial view, high angle", 0, "" },
    { SC_COMP, "retrato|portrait shot|busto", "portrait framing, shallow depth of field", 0, "" },
    { SC_COMP, "simetric|symmetr", "symmetrical composition, centered", 0, "" },
    { SC_COMP, "perspectiva|perspective", "strong perspective, leading lines", 0, "" },
    { SC_COMP, "de baixo|low angle|contra-plongee", "low angle shot", 0, "" },
    { SC_CAMERA, "35mm", "shot on 35mm lens", 0, "" },
    { SC_CAMERA, "85mm", "shot on 85mm lens, shallow depth of field", 0, "" },
    { SC_CAMERA, "full-frame|full frame", "full-frame camera", 0, "" },
    { SC_CAMERA, "profundidade de campo|depth of field|bokeh|desfoque", "shallow depth of field, bokeh", 0, "" },
    { SC_CAMERA, "macro", "macro lens, extreme detail", 0, "" },
    { SC_QUALITY, "alta qualidade|high quality|profissional|professional", "high quality, sharp focus", 0, "" },
    { SC_QUALITY, "detalhad|detailed|hiper|ultra", "highly detailed, intricate textures", 0, "" },
    { SC_QUALITY, "4k|8k|alta resolucao|high resolution|hd", "high resolution, 4k", 0, "" },
    { SC_QUALITY, "textura|texture", "visible material textures", 0, "" },
    { SC_ASPECT, "vertical|retrato|portrait|em pe", "", 0.75, "" },
    { SC_ASPECT, "horizontal|paisagem|landscape|deitado", "", 1.3333, "" },
    { SC_ASPECT, "quadrad|square|1:1", "", 1.0, "" },
    { SC_ASPECT, "wallpaper de celular|papel de parede de celular|phone wallpaper|stories|story|reels|9:16", "", 0.5625, "wallpaper" },
    { SC_ASPECT, "wallpaper|papel de parede|desktop wallpaper|16:9|widescreen|banner|capa|thumbnail", "", 1.7778, "banner" },
    { SC_ASPECT, "foto de perfil|profile picture|avatar", "", 1.0, "profile" },
    { SC_ASPECT, "3:2", "", 1.5, "" },
    { SC_ASPECT, "4:3", "", 1.3333, "" },
    { SC_ASPECT, "3:4", "", 0.75, "" },
    { SC_ASPECT, "21:9|ultrawide", "", 2.3333, "" },
};
#define SDS_NC ((int)(sizeof SDS_C / sizeof SDS_C[0]))


/* exemplos naturais (PT e EN) de cada conceito, para o embedding: a clausula do pedido e comparada com eles, nao com as chaves de palavras.
 * A pontuacao do conceito e a do melhor exemplo. "phrase" tem de ser igual a de SDS_C. */
static const struct { const char *phrase, *ex; } SDS_X[] = {
    { "golden hour, warm low sun", "luz rasante de fim de tarde;sol baixo e dourado;por do sol;golden hour light;warm low sunset sun" },
    { "sunrise, soft warm light", "luz suave do amanhecer;nascer do sol;sunrise glow;early morning light" },
    { "night scene, dark sky, artificial lights", "cena noturna;a noite com luzes acesas;night scene;dark sky at night" },
    { "soft natural daylight", "luz natural do dia;iluminacao natural suave;soft natural daylight;sunlit by the window" },
    { "dramatic lighting, rim light", "iluminacao dramatica;contraluz forte;dramatic lighting;strong backlight and shadows" },
    { "cinematic lighting, dramatic contrast", "iluminacao cinematografica;luz de cinema com contraste;cinematic lighting" },
    { "professional studio lighting, softbox", "iluminacao de estudio;luz de softbox;studio lighting" },
    { "warm cozy lighting, 3000K", "luz quente e aconchegante;iluminacao amarelada;warm cozy light" },
    { "soft diffused light", "luz difusa e suave;dia nublado;soft diffused light;overcast light" },
    { "overcast diffused light, soft mist", "dia nublado com neblina;nevoeiro;foggy overcast day" },
    { "neon lighting, glowing colors", "luzes de neon;neon colorido brilhando;neon lights" },
    { "close-up shot", "plano fechado;close no detalhe;close-up shot" },
    { "wide shot, wide angle", "plano aberto;grande angular;wide angle shot;wide establishing view" },
    { "aerial view, high angle", "vista aerea;visto de cima;drone view;aerial view" },
    { "low angle shot", "visto de baixo para cima;contra-plongee;low angle shot" },
    { "symmetrical composition, centered", "composicao simetrica e centralizada;symmetrical centered composition" },
    { "strong perspective, leading lines", "perspectiva marcada com linhas de fuga;strong perspective leading lines" },
    { "shallow depth of field, bokeh", "pouca profundidade de campo com fundo desfocado;shallow depth of field;bokeh background" },
    { "professional photograph, photorealistic", "fotografia realista;foto profissional;photorealistic photograph" },
    { "cinematic film still, anamorphic, film grain", "quadro de filme;aparencia de cinema com grao;cinematic film still" },
    { "minimalist, clean, uncluttered", "estilo minimalista e limpo;minimalist clean design" },
    { "luxurious, elegant, refined", "luxuoso e sofisticado;acabamento de alto padrao;luxurious elegant refined" },
    { "vintage, retro look", "aparencia retro;estilo vintage;retro vintage look" },
    { "futuristic, sci-fi design", "design futurista;ficcao cientifica;futuristic sci-fi design" },
    { "highly detailed, intricate textures", "muito detalhado;texturas ricas e detalhes;highly detailed intricate textures" },
    { "visible material textures", "texturas dos materiais visiveis;grao da madeira e trama do tecido;visible material textures" },
    { "high quality, sharp focus", "alta qualidade e foco nitido;high quality sharp focus" },
};

/* lexico PT -> EN do conteudo (assunto e ambiente); radicais sem acento. "" = palavra funcional, descartada */
static const struct { const char *pt, *en; } SDS_LEX[] = {
    { "piscina", "swimming pool" }, { "fachada", "facade" }, { "pavimentos", "stories" }, { "pavimento", "story" }, { "dois", "two" }, { "tres", "three" }, { "tropical", "tropical" }, { "aparente", "exposed" },
    { "varanda", "balcony" }, { "telhado", "roof" }, { "garagem", "garage" }, { "paisagismo", "landscaping" }, { "vegetacao", "vegetation" }, { "arvores", "trees" }, { "arvore", "tree" }, { "flores", "flowers" },
    { "tenis", "sneaker" }, { "garrafa", "bottle" }, { "relogio", "watch" }, { "perfume", "perfume bottle" }, { "bolsa", "handbag" }, { "campanha", "" }, { "publicitaria", "" }, { "publicidade", "" }, { "produto", "product" },
    { "futurista", "futuristic" }, { "esportiva", "sports" }, { "premium", "premium" }, { "moderna", "modern" }, { "grandes", "large" }, { "portas", "doors" }, { "janelas", "windows" },
    { "curvo", "curved" }, { "curva", "curved" }, { "clara", "light" }, { "claro", "light" }, { "escuro", "dark" }, { "escura", "dark" }, { "acinzentada", "greyish" }, { "acinzentado", "greyish" }, { "logotipo", "logo" }, { "logo", "logo" },
    { "arcos", "arches" }, { "arco", "arch" }, { "corredor", "corridor" }, { "objetos", "objects" }, { "objeto", "object" }, { "suave", "soft" }, { "lateral", "side" }, { "spots", "recessed spotlights" }, { "perfil", "profile" },
    { "led", "LED" }, { "tapete", "rug" }, { "altura", "" }, { "olhos", "" }, { "aberto", "wide" }, { "plano", "" }, { "camera", "camera" }, { "dourada", "golden" }, { "ripada", "slatted" }, { "ripado", "slatted" }, { "brises", "louvers" },
    { "crie", "" }, { "renderizacao", "" }, { "render", "" }, { "visualizacao", "" }, { "cena", "scene" }, { "aparencia", "" }, { "estilo", "" }, { "iluminacao", "" }, { "formato", "" }, { "criar", "" }, { "gere", "" }, { "gerar", "" }, { "faca", "" }, { "fazer", "" }, { "uma", "" }, { "um", "" }, { "de", "" }, { "do", "" }, { "da", "" }, { "em", "in" }, { "no", "in the" }, { "na", "in the" },
    { "com", "with" }, { "para", "" }, { "e", "and" }, { "o", "" }, { "a", "" }, { "os", "" }, { "as", "" }, { "que", "" }, { "usando", "wearing" }, { "vestindo", "wearing" }, { "imagem", "" }, { "foto", "" }, { "fotografia", "" },
    { "homem", "man" }, { "mulher", "woman" }, { "menino", "boy" }, { "menina", "girl" }, { "crianca", "child" }, { "pessoa", "person" }, { "idoso", "elderly man" }, { "idosa", "elderly woman" },
    { "cachorro", "dog" }, { "gato", "cat" }, { "passaro", "bird" }, { "cavalo", "horse" }, { "leao", "lion" }, { "lobo", "wolf" }, { "dragao", "dragon" }, { "coelho", "rabbit" },
    { "carro", "car" }, { "esportivo", "sports" }, { "moto", "motorcycle" }, { "aviao", "airplane" }, { "navio", "ship" }, { "trem", "train" }, { "bicicleta", "bicycle" },
    { "casa", "house" }, { "predio", "building" }, { "cidade", "city" }, { "rua", "street" }, { "estrada", "road" }, { "floresta", "forest" }, { "montanha", "mountain" }, { "praia", "beach" }, { "mar", "sea" }, { "rio", "river" },
    { "lago", "lake" }, { "deserto", "desert" }, { "campo", "countryside" }, { "jardim", "garden" }, { "ceu", "sky" }, { "nuvens", "clouds" }, { "chuva", "rain" }, { "neve", "snow" }, { "sol", "sun" }, { "lua", "moon" }, { "estrelas", "stars" },
    { "escritorio", "office" }, { "sala", "living room" }, { "quarto", "bedroom" }, { "cozinha", "kitchen" }, { "banheiro", "bathroom" }, { "recepcao", "reception" }, { "clinica", "clinic" }, { "loja", "store" }, { "restaurante", "restaurant" }, { "cafe", "cafe" },
    { "mesa", "table" }, { "cadeira", "chair" }, { "poltrona", "armchair" }, { "sofa", "sofa" }, { "cama", "bed" }, { "balcao", "counter" }, { "espelho", "mirror" }, { "janela", "window" }, { "porta", "door" }, { "parede", "wall" }, { "piso", "floor" }, { "teto", "ceiling" },
    { "tapete", "rug" }, { "planta", "plant" }, { "plantas", "plants" }, { "quadro", "framed picture" }, { "luminaria", "lamp" }, { "estante", "bookshelf" }, { "livro", "book" }, { "livros", "books" },
    { "terno", "suit" }, { "camisa", "shirt" }, { "vestido", "dress" }, { "chapeu", "hat" }, { "oculos", "glasses" },
    { "preto", "black" }, { "preta", "black" }, { "branco", "white" }, { "branca", "white" }, { "vermelho", "red" }, { "vermelha", "red" }, { "azul", "blue" }, { "verde", "green" }, { "amarelo", "yellow" }, { "amarela", "yellow" },
    { "laranja", "orange" }, { "roxo", "purple" }, { "rosa", "pink" }, { "marrom", "brown" }, { "cinza", "grey" }, { "dourado", "golden" }, { "prata", "silver" }, { "bege", "beige" }, { "vinho", "burgundy" },
    { "madeira", "wood" }, { "vidro", "glass" }, { "metal", "metal" }, { "pedra", "stone" }, { "marmore", "marble" }, { "concreto", "concrete" }, { "tecido", "fabric" }, { "couro", "leather" }, { "ouro", "gold" },
    { "grande", "large" }, { "pequeno", "small" }, { "pequena", "small" }, { "alto", "tall" }, { "baixo", "low" }, { "velho", "old" }, { "antigo", "ancient" }, { "novo", "new" }, { "bonito", "beautiful" }, { "bonita", "beautiful" },
    { "feliz", "happy" }, { "triste", "sad" }, { "sorrindo", "smiling" }, { "sentado", "sitting" }, { "sentada", "sitting" }, { "andando", "walking" }, { "correndo", "running" }, { "dormindo", "sleeping" },
    { "sobre", "on" }, { "ao", "at the" }, { "lado", "side" }, { "fundo", "background" }, { "frente", "front" }, { "entre", "between" }, { "ao redor", "around" },
};
#define SDS_NL ((int)(sizeof SDS_LEX / sizeof SDS_LEX[0]))

/* texto -> minusculas sem acento (UTF-8 latino) */
static void sds_fold(const char *s, char *out, size_t cap) {
    size_t n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p && n + 1 < cap; p++) {
        if (*p == 0xC3 && p[1]) {
            const unsigned char c = (unsigned char)(p[1] | 0x20);            /* maiusculas acentuadas viram minusculas */
            char r = '?';
            if (c >= 0xA0 && c <= 0xA5) r = 'a'; else if (c == 0xA7) r = 'c'; else if (c >= 0xA8 && c <= 0xAB) r = 'e'; else if (c >= 0xAC && c <= 0xAF) r = 'i';
            else if (c >= 0xB2 && c <= 0xB6) r = 'o'; else if (c >= 0xB9 && c <= 0xBC) r = 'u';
            out[n++] = r; p++; continue;
        }
        if (*p >= 0x80) { out[n++] = ' '; continue; }
        out[n++] = (char)tolower(*p);
    }
    out[n] = 0;
}

static int sds_isw(char c) { return isalnum((unsigned char)c) || c == '-' || c == ':'; }

/* a chave (radical) esta no texto dobrado, no inicio de uma palavra? chaves de ate 3 letras tambem exigem o fim da palavra */
static int sds_key_in(const char *t, const char *key, size_t kl) {
    for (const char *p = t; (p = strstr(p, key)) != NULL; p++) {
        if (p != t && sds_isw(p[-1])) continue;
        if ((kl <= 3 || memchr(key, ' ', kl)) && sds_isw(p[kl])) continue;
        return 1;
    }
    return 0;
}
static int sds_match(const char *t, const char *keys) {
    char k[160];
    for (const char *p = keys; *p; ) {
        const char *e = strchr(p, '|'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l >= sizeof k) l = sizeof k - 1;
        memcpy(k, p, l); k[l] = 0;
        if (sds_key_in(t, k, l)) return 1;
        p += l; if (*p == '|') p++;
    }
    return 0;
}

/* apaga (vira espacos) as palavras do texto que casam com as chaves: o que sobra da clausula e conteudo */
static void sds_blank(char *t, const char *keys) {
    char k[160];
    for (const char *p = keys; *p; ) {
        const char *e = strchr(p, '|'); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l >= sizeof k) l = sizeof k - 1;
        memcpy(k, p, l); k[l] = 0;
        for (char *q = t; l && (q = strstr(q, k)) != NULL; q++) {
            if (q != t && sds_isw(q[-1])) continue;
            if ((l <= 3 || strchr(k, ' ')) && sds_isw(q[l])) continue;
            char *end = q + l; while (sds_isw(*end)) end++;
            memset(q, ' ', (size_t)(end - q));
        }
        p += l; if (*p == '|') p++;
    }
}

static void sds_cat(char *dst, size_t cap, const char *add) {
    if (!*add || strstr(dst, add)) return;
    if (*dst) strncat(dst, ", ", cap - strlen(dst) - 1);
    strncat(dst, add, cap - strlen(dst) - 1);
}
/* junta uma lista "a, b, c" sem repetir itens ja presentes */
static void sds_cat_list(char *dst, size_t cap, const char *list) {
    char it[160];
    for (const char *p = list; *p; ) {
        const char *e = strstr(p, ", "); size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l >= sizeof it) l = sizeof it - 1;
        memcpy(it, p, l); it[l] = 0;
        int dup = 0;
        for (const char *q = dst; (q = strstr(q, it)) != NULL; q++) if ((q == dst || q[-1] == ' ') && (q[l] == 0 || q[l] == ',')) { dup = 1; break; }
        if (!dup && l) sds_cat(dst, cap, it);
        p += l; if (*p == ',') p++; while (*p == ' ') p++;
    }
}
static char *sds_field(SdSpec *s, int cat, size_t *cap) {
    switch (cat) {
    case SC_STYLE: *cap = sizeof s->style; return s->style;
    case SC_LIGHT: *cap = sizeof s->lighting; return s->lighting;
    case SC_COMP: *cap = sizeof s->composition; return s->composition;
    case SC_CAMERA: *cap = sizeof s->camera; return s->camera;
    case SC_QUALITY: *cap = sizeof s->quality; return s->quality;
    default: *cap = sizeof s->extra; return s->extra;
    }
}
static void sds_apply(SdSpec *s, const SdsConcept *c) {
    if (c->cat == SC_ASPECT) { if (s->aspect == 0 || c->purpose[0]) { s->aspect = c->aspect; } if (c->purpose[0]) snprintf(s->purpose, sizeof s->purpose, "%s", c->purpose); return; }
    size_t cap; char *f = sds_field(s, c->cat, &cap);
    sds_cat(f, cap, c->phrase);
}

/* palavras do conteudo que, em portugues, vem depois do substantivo e em ingles vao antes (cores, tamanho, idade, estilo) */
static int sds_adj(const char *w) {
    static const char *const A[] = { "black", "white", "red", "blue", "green", "yellow", "orange", "purple", "pink", "brown", "grey", "golden", "silver", "beige", "burgundy", "large", "small", "tall", "low", "old", "ancient", "new",
        "two-story", "three-story", "concrete", "wood", "glass", "stone", "marble", "leather", "beautiful", "happy", "sad", "modern", "tropical", "futuristic", "sports", "premium", "exposed", "elderly" };
    for (size_t i = 0; i < sizeof A / sizeof A[0]; i++) if (!strcmp(A[i], w)) return 1;
    return 0;
}
static int sds_conn(const char *w) { return !strcmp(w, "in") || !strcmp(w, "with") || !strcmp(w, "and") || !strcmp(w, "wearing") || !strcmp(w, "on") || !strcmp(w, "at") || !strcmp(w, "the") || !strcmp(w, "side") || !strcmp(w, "between"); }

/* conteudo de uma clausula (ja dobrada): palavra a palavra pelo lexico; desconhecida: se parece ingles (ja estava em ingles) segue, senao vai a unknown */
static void sds_content(SdSpec *s, const char *clause, char *out, size_t cap) {
    char w[64]; size_t n = 0; out[0] = 0;
    char words[40][48]; int nw = 0;
    char cl2[400]; snprintf(cl2, sizeof cl2, "%s", clause);
    static const char *const fl[][2] = { { "dois pavimentos", "two-story" }, { "dois andares", "two-story" }, { "tres pavimentos", "three-story" }, { "tres andares", "three-story" } };
    for (int k = 0; k < 4; k++) { char *q = strstr(cl2, fl[k][0]); if (q) { const size_t a = strlen(fl[k][0]), b = strlen(fl[k][1]); memmove(q + b, q + a, strlen(q + a) + 1); memcpy(q, fl[k][1], b); } }
    for (const char *p = cl2; ; p++) {
        if (sds_isw(*p) && *p != ':' && n + 1 < sizeof w) { w[n++] = *p; continue; }
        if (n) {
            w[n] = 0; n = 0;
            int hit = -1;
            for (int i = 0; i < SDS_NL; i++) if (!strcmp(SDS_LEX[i].pt, w)) { hit = i; break; }
            const char *en = NULL;
            if (hit >= 0) en = SDS_LEX[hit].en;
            else {                                                       /* sem traducao: palavra em ingles passa; em portugues fica anotada */
                const size_t l = strlen(w);
                const int pt = l > 3 && (strchr("ao", w[l - 1]) || !strcmp(w + l - 2, "os") || !strcmp(w + l - 2, "as") || !strcmp(w + l - 2, "ao") || !strcmp(w + l - 3, "ado") || !strcmp(w + l - 3, "ida") || !strcmp(w + l - 3, "dor"));
                if (pt && !strstr(s->unknown, w) && strlen(s->unknown) + l + 2 < sizeof s->unknown) { if (*s->unknown) strcat(s->unknown, " "); strcat(s->unknown, w); }
                en = w;
            }
            if (en[0] && nw < 40) {
                snprintf(words[nw], sizeof words[0], "%s", en);
                if (nw > 0 && sds_adj(words[nw]) && !sds_adj(words[nw - 1]) && !sds_conn(words[nw - 1])) { char t[48]; memcpy(t, words[nw], 48); memcpy(words[nw], words[nw - 1], 48); memcpy(words[nw - 1], t, 48); }
                nw++;
            }
        }
        if (!*p) break;
    }
    while (nw > 0 && sds_conn(words[nw - 1])) nw--;                      /* conector sobrando no fim ("in office, with") */
    for (int i = 0; i < nw; i++) { if (*out) strncat(out, " ", cap - strlen(out) - 1); strncat(out, words[i], cap - strlen(out) - 1); }
}


/* ---------- roteador de intencao e compilador de cena por dominio (presets) ---------- */
typedef struct { const char *domain, *scene, *keys; const char *style, *composition, *lighting, *camera, *negative; double aspect; } SdsDomain;
static const SdsDomain SDS_D[] = {
    { "architecture", "exterior", "casa|house|fachada|facade|predio|building|residencia|residence|edificio|piscina|pool|exterior|villa|sobrado", "photorealistic architectural exterior visualization, archviz",
      "three-quarter front view, eye level, balanced composition", "soft natural daylight, clear sky", "architectural wide angle lens, straight vertical lines", "people, cars, text, distorted perspective, tilted verticals, cluttered", 1.5 },
    { "architecture", "interior", "sala|living room|recepcao|reception|quarto|bedroom|cozinha|kitchen|banheiro|bathroom|escritorio interior|interior|loja|restaurante|ambiente", "photorealistic architectural interior photograph",
      "wide view, eye level, balanced composition", "soft natural daylight with warm artificial accents", "architectural wide angle lens, straight vertical lines", "people, text, distorted perspective, cluttered, cgi look", 1.5 },
    { "product", "hero", "tenis|sneaker|produto|product|garrafa|bottle|relogio|watch|perfume|bolsa|handbag|campanha|campaign|embalagem|packshot", "premium commercial product photograph",
      "hero product shot, centered, product first, minimal uncluttered background", "controlled studio lighting, soft reflections, defined shadows", "85mm lens, shallow depth of field", "text, logo, watermark, clutter, extra objects, distorted shape", 1.0 },
    { "person", "portrait", "homem|mulher|pessoa|menino|menina|crianca|idoso|man |woman|person|retrato|portrait|modelo", "professional portrait photograph",
      "medium shot, eye level, subject centered", "soft flattering light", "85mm lens, shallow depth of field", "bad anatomy, extra fingers, deformed face, text", 0.75 },
    { "landscape", "wide", "paisagem|landscape|montanha|mountain|praia|beach|floresta|forest|lago|lake|deserto|desert|campo|cidade|city|rua|street|ceu|sky", "photorealistic landscape photograph",
      "wide establishing shot, strong depth, rule of thirds", "natural light, atmospheric depth", "wide angle lens", "people, text, oversaturated, lowres", 1.5 },
};
#define SDS_ND ((int)(sizeof SDS_D / sizeof SDS_D[0]))

/* classificacao rapida por pontuacao de palavras; o assunto (primeira clausula) pesa mais; sem acerto, "general" */
static const SdsDomain *sds_route(const char *folded, const char *first_clause) {
    int best = -1, bs = 0;
    for (int i = 0; i < SDS_ND; i++) {
        char k[160]; int sc = 0;
        for (const char *p = SDS_D[i].keys; *p; ) {
            const char *e = strchr(p, '|'); size_t l = e ? (size_t)(e - p) : strlen(p);
            if (l >= sizeof k) l = sizeof k - 1; memcpy(k, p, l); k[l] = 0;
            if (sds_key_in(folded, k, l)) sc += 1;
            if (first_clause && sds_key_in(first_clause, k, l)) sc += 2;
            p += l; if (*p == '|') p++;
        }
        if (sc > bs) { bs = sc; best = i; }
    }
    return best >= 0 ? &SDS_D[best] : NULL;
}

/* ---------- 1. interpretacao ---------- */
static void sds_negative_from(SdSpec *s, const char *folded) {           /* "sem X", "without X", "nao quero X" */
    static const char *const cue[] = { " sem ", " without ", " no ", " nao quero ", " evite " };
    for (int k = 0; k < 5; k++) {
        const char *p = strstr(folded, cue[k]);
        if (!p) continue;
        p += strlen(cue[k]);
        char cl[100]; size_t n = 0; while (*p && *p != ',' && *p != '.' && *p != ';' && n + 1 < sizeof cl) cl[n++] = *p++;
        cl[n] = 0;
        char en[100]; sds_content(s, cl, en, sizeof en);
        if (en[0] && strlen(s->negative) + strlen(en) + 3 < sizeof s->negative) { if (*s->negative) strcat(s->negative, ", "); strcat(s->negative, en); }
    }
}

/* clausula sem palavra de conceito: o RAG compara a clausula com os exemplos de cada conceito (embedding multilingue); o conceito vale o melhor
 * exemplo. Entra so se passa do limiar absoluto E tem folga sobre o melhor conceito DIFERENTE. 1 = aplicou algum. KATANA_EMBED_DEBUG mostra os cossenos. */
static int sds_rag(SdSpec *s, const char *clause) {
    static const char *chunk[256]; static int owner[256], n;
    if (!n) {
        static char buf[256][160];
        for (size_t x = 0; x < sizeof SDS_X / sizeof SDS_X[0]; x++) {
            int ci = -1; for (int i = 0; i < SDS_NC; i++) if (!strcmp(SDS_C[i].phrase, SDS_X[x].phrase)) { ci = i; break; }
            if (ci < 0) continue;
            for (const char *p = SDS_X[x].ex; *p && n < 256; ) {
                const char *e = strchr(p, ';'); size_t l = e ? (size_t)(e - p) : strlen(p);
                if (l >= sizeof buf[0]) l = sizeof buf[0] - 1;
                memcpy(buf[n], p, l); buf[n][l] = 0; chunk[n] = buf[n]; owner[n++] = ci;
                p += l; if (*p == ';') p++;
            }
        }
    }
    float sc[256];
    if (!es_available() || es_rank(clause, EMB_SEARCH, chunk, NULL, n, 8000.0, sc) <= 0) return 0;
    int b1 = -1, b2 = -1;
    for (int i = 0; i < n; i++) if (b1 < 0 || sc[i] > sc[b1]) b1 = i;
    for (int i = 0; i < n; i++) if (owner[i] != owner[b1] && (b2 < 0 || sc[i] > sc[b2])) b2 = i;
    const float lim = getenv("KATANA_SPEC_RAG") ? (float)atof(getenv("KATANA_SPEC_RAG")) : 0.82f, gap = getenv("KATANA_SPEC_GAP") ? (float)atof(getenv("KATANA_SPEC_GAP")) : 0.05f;
    if (getenv("KATANA_EMBED_DEBUG")) fprintf(stderr, "[spec] '%s' -> %s (%.3f) contra %s (%.3f)\n", clause, SDS_C[owner[b1]].phrase, sc[b1], b2 >= 0 ? SDS_C[owner[b2]].phrase : "-", b2 >= 0 ? sc[b2] : 0.0f);
    if (sc[b1] >= lim && (b2 < 0 || sc[b1] - sc[b2] >= gap)) { sds_apply(s, &SDS_C[owner[b1]]); return 1; }
    return 0;
}

static void sds_parse(SdSpec *s, const char *text, int has_image, int use_rag) {
    memset(s, 0, sizeof *s);
    snprintf(s->request, sizeof s->request, "%s", text); s->rag = use_rag;
    snprintf(s->intent, sizeof s->intent, "%s", has_image ? "image_to_image" : "text_to_image");
    char folded[4096]; sds_fold(text, folded, sizeof folded);
    char padded[4200]; snprintf(padded, sizeof padded, " %s", folded);
    if (sds_match(padded, "remova|remove |apague|erase|tire o|tire a|troque o fundo|change the background|mantenha a pessoa|keep the person")) snprintf(s->intent, sizeof s->intent, "inpaint");
    sds_negative_from(s, padded);
    char *cl = strdup(folded), *save = NULL; (void)save;
    for (char *p = cl; *p; p++) if (*p == ';' || *p == '\n' || *p == '.') *p = ',';
    char subject_parts[240] = "", env_parts[240] = "";
    int first = 1;
    for (char *t = cl; t && *t; ) {
        char *e = strchr(t, ','); if (e) *e = 0;
        char clause[300]; snprintf(clause, sizeof clause, "%s", t);
        t = e ? e + 1 : NULL;
        while (*clause == ' ') memmove(clause, clause + 1, strlen(clause));
        if (!clause[0]) continue;
        int hit = 0;
        char raw[300]; snprintf(raw, sizeof raw, "%s", clause);
        for (int i = 0; i < SDS_NC; i++) if (sds_match(raw, SDS_C[i].keys)) { sds_apply(s, &SDS_C[i]); sds_blank(clause, SDS_C[i].keys); hit++; }
        char *sem = strstr(clause, " sem "); if (sem) *sem = 0;           /* o que vem depois de "sem" ja foi para o negativo */
        char en[300]; sds_content(s, clause, en, sizeof en);
        if (!hit && use_rag && strlen(clause) > 3 && !first) { if (sds_rag(s, clause)) continue; }
        if (!en[0]) { first = 0; continue; }
        if (first) { sds_cat(subject_parts, sizeof subject_parts, en); first = 0; }
        else if (strstr(en, "in ") == en || strstr(en, "in the") == en || strstr(en, "at the") == en || strstr(en, "on ") == en || strstr(en, "with ") == en) sds_cat(env_parts, sizeof env_parts, en);
        else sds_cat(env_parts, sizeof env_parts, en);
    }
    free(cl);
    { char fc[300]; snprintf(fc, sizeof fc, "%s", folded); char *c = strchr(fc, ','); if (c) *c = 0;
      const SdsDomain *d = sds_route(padded, fc);
      snprintf(s->domain, sizeof s->domain, "%s", d ? d->domain : "general"); snprintf(s->scene, sizeof s->scene, "%s", d ? d->scene : "");
      if (d) {                                                              /* o compilador de cena preenche so o que o pedido nao disse */
          if (!strstr(s->style, "photograph")) { char st[200]; snprintf(st, sizeof st, "%s%s%s", d->style, s->style[0] ? ", " : "", s->style); snprintf(s->style, sizeof s->style, "%s", st); }
          if (!s->composition[0]) snprintf(s->composition, sizeof s->composition, "%s", d->composition);
          if (!s->lighting[0]) snprintf(s->lighting, sizeof s->lighting, "%s", d->lighting);
          if (!s->camera[0]) snprintf(s->camera, sizeof s->camera, "%s", d->camera);
          if (s->aspect == 0 && strcmp(d->domain, "person")) s->aspect = d->aspect;
          sds_cat_list(s->negative, sizeof s->negative, d->negative);
      } }
    snprintf(s->subject, sizeof s->subject, "%s", subject_parts); snprintf(s->environment, sizeof s->environment, "%s", env_parts);
    s->person = sds_match(padded, "homem|mulher|pessoa|menino|menina|crianca|retrato|man |woman|person|people|portrait|girl|boy");
    int nw = 0; for (const char *p = padded; *p; p++) if (*p == ' ') nw++;
    (void)nw;
}

/* ---------- JSON: escrita ---------- */
static void sds_json(const SdSpec *s, FILE *f) {
    const struct { const char *k; const char *v; } F[] = { { "intent", s->intent }, { "domain", s->domain }, { "scene", s->scene }, { "subject", s->subject }, { "environment", s->environment }, { "style", s->style }, { "lighting", s->lighting },
        { "composition", s->composition }, { "camera", s->camera }, { "quality", s->quality }, { "negative", s->negative }, { "purpose", s->purpose } };
    fputs("{\n", f);
    for (int i = 0; i < 12; i++) { fprintf(f, "  \"%s\": \"", F[i].k); for (const char *p = F[i].v; *p; p++) { if (*p == '"' || *p == '\\') fputc('\\', f); fputc(*p, f); } fputs("\",\n", f); }
    if (s->aspect > 0) fprintf(f, "  \"requested_aspect_ratio\": %.4f\n}\n", s->aspect); else fputs("  \"requested_aspect_ratio\": null\n}\n", f);
}

/* ---------- 2. configuracao (codigo) e 4. adaptador SD ---------- */
typedef struct { char positive[2400], negative[900]; int w, h, steps, lcm; double guidance, strength; int chunks_used; } SdsPlan;

static void sds_plan(const SdSpec *s, int has_image, int ref_w, int ref_h, int chunks, int lcm, double longside, const SdClip *clip, SdsPlan *o) {
    memset(o, 0, sizeof *o);
    /* configuracao: proporcao pedida, senao a da referencia, senao 1:1 (3:2 em cena aberta) */
    double ar = s->aspect > 0 ? s->aspect : (has_image && ref_w > 0 && ref_h > 0) ? (double)ref_w / ref_h : 1.0;
    if (ar >= 1.0) { o->w = sdp_round64(longside); o->h = sdp_round64(longside / ar); } else { o->h = sdp_round64(longside); o->w = sdp_round64(longside * ar); }
    o->lcm = lcm; o->steps = lcm ? 4 : 25; o->guidance = lcm ? 1.0 : 7.5; o->strength = has_image ? 0.55 : 1.0;
    /* adaptador SD: ordem de importancia, assunto com peso */
    const int budget = chunks * 75 - 4;
    char pr[2400] = "";
    {
        char subj[300]; if (s->subject[0]) snprintf(subj, sizeof subj, "(%s:1.2)", s->subject); else subj[0] = 0;
        /* partes: estilo, assunto, cada item do ambiente (separado por virgula), composicao, luz, camera, qualidade, extra */
        enum { NP = 40 };
        const char *parts[NP]; char envbuf[300]; int np = 0;
        parts[np++] = s->style; parts[np++] = subj;
        snprintf(envbuf, sizeof envbuf, "%s", s->environment);
        for (char *it = envbuf; *it && np < NP - 5; ) { char *e = strstr(it, ", "); if (e) *e = 0; if (*it) parts[np++] = it; if (!e) break; it = e + 2; }
        parts[np++] = s->composition; parts[np++] = s->lighting; parts[np++] = s->camera; parts[np++] = s->quality; parts[np++] = s->extra;
        int use[NP], nu = 0, taken[NP] = { 0 };
        char all[2400] = ""; for (int i = 0; i < np; i++) if (parts[i][0]) { use[nu++] = i; snprintf(all + strlen(all), sizeof all - strlen(all), "%s%s", all[0] ? ", " : "", parts[i]); }
        /* priorizacao: o assunto e o estilo entram sempre; cabendo tudo no orcamento, a ordem e a canonica e o embedding nem e chamado (custo zero);
         * so quando passa do orcamento o embedding ordena o resto pela proximidade com o pedido inteiro, e entra o que couber, do mais ao menos proximo */
        int order[NP], no = 0;
        order[no++] = 1; order[no++] = 0;
        if (sdb_tokens(clip, all) > budget && s->rag && s->request[0]) {
            const char *ch[NP]; int idx[NP], nc = 0; float sc[NP];
            for (int k = 0; k < nu; k++) if (use[k] != 0 && use[k] != 1) { ch[nc] = parts[use[k]]; idx[nc++] = use[k]; }
            if (nc && es_available() && es_rank(s->request, EMB_SEARCH, ch, NULL, nc, 4000.0, sc) > 0) {
                for (int a2 = 0; a2 < nc; a2++) for (int b2 = a2 + 1; b2 < nc; b2++) if (sc[b2] > sc[a2]) { float t = sc[a2]; sc[a2] = sc[b2]; sc[b2] = t; int ti = idx[a2]; idx[a2] = idx[b2]; idx[b2] = ti; }
                for (int k = 0; k < nc; k++) order[no++] = idx[k];
            } else for (int k = 2; k < np; k++) order[no++] = k;
        } else for (int k = 2; k < np; k++) order[no++] = k;
        for (int k = 0; k < no; k++) {
            const int i = order[k]; if (!parts[i][0]) continue;
            char trial[2400]; snprintf(trial, sizeof trial, "%s%s%s", pr, pr[0] ? ", " : "", parts[i]);
            if (i == 1 || sdb_tokens(clip, trial) <= budget) { snprintf(pr, sizeof pr, "%s", trial); taken[i] = 1; }
        }
        pr[0] = 0;                                                            /* sai na ordem canonica, so o que entrou */
        for (int i = 0; i < np; i++) if (taken[i]) snprintf(pr + strlen(pr), sizeof pr - strlen(pr), "%s%s", pr[0] ? ", " : "", parts[i]);
    }
    snprintf(o->positive, sizeof o->positive, "%s", pr);
    o->chunks_used = (sdb_tokens(clip, pr) + 74) / 75;
    /* negativo coerente com o estilo */
    char ng[900] = "lowres, blurry, jpeg artifacts, watermark, text, signature, deformed";
    const int anime = strstr(s->style, "anime") != NULL, paint = strstr(s->style, "painting") || strstr(s->style, "pencil") || strstr(s->style, "illustration") || strstr(s->style, "watercolor");
    const int photo = !anime && !paint && !strstr(s->style, "3d");
    if (photo) sds_cat_list(ng, sizeof ng, "cartoon, painting, illustration, drawing, cgi, oversaturated");
    if (anime) sds_cat(ng, sizeof ng, "photo, realistic, 3d");
    if (s->person) sds_cat_list(ng, sizeof ng, "bad anatomy, extra fingers, extra limbs, disfigured face, bad hands");
    if (s->negative[0]) sds_cat_list(ng, sizeof ng, s->negative);
    snprintf(o->negative, sizeof o->negative, "%s", ng);
}

/* Prompt do render final (FLUX ou Krea): esses motores leem prosa, sem limite de 75 tokens nem pesos; o mesmo SdSpec vira frases */
static void sds_prose(const SdSpec *s, char *out, size_t cap) {
    out[0] = 0;
    const struct { const char *pre, *v; } P[] = { { "A ", s->style }, { "Subject: ", s->subject }, { "Setting: ", s->environment }, { "Composition: ", s->composition },
        { "Lighting: ", s->lighting }, { "Camera: ", s->camera }, { "", s->quality } };
    for (int i = 0; i < 7; i++) if (P[i].v[0]) { strncat(out, P[i].pre, cap - strlen(out) - 1); strncat(out, P[i].v, cap - strlen(out) - 1); strncat(out, ". ", cap - strlen(out) - 1); }
    if (s->negative[0]) { strncat(out, "Avoid: ", cap - strlen(out) - 1); strncat(out, s->negative, cap - strlen(out) - 1); strncat(out, ".", cap - strlen(out) - 1); }
}

static int sds_main(int argc, char **argv) {
    const char *text = NULL, *image = NULL; int chunks = 4, lcm = 0, json = 0, norag = 0, final_mode = 0; const char *fengine = "flux"; double longside = 512; const char *brief = NULL;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i]; const int more = i + 1 < argc;
        if (!strcmp(a, "--ref") && more) image = argv[++i];
        else if (!strcmp(a, "--chunks") && more) chunks = atoi(argv[++i]);
        else if (!strcmp(a, "--long") && more) longside = atof(argv[++i]);
        else if (!strcmp(a, "--lcm")) lcm = 1;
        else if (!strcmp(a, "--json")) json = 1;
        else if (!strcmp(a, "--no-rag")) norag = 1;
        else if (!strcmp(a, "--final")) final_mode = 1;
        else if (!strcmp(a, "--final-engine") && more) fengine = argv[++i];
        else if (!strcmp(a, "--brief") && more) brief = argv[++i];
        else if (!strcmp(a, "--engine") && more) fengine = argv[++i];
        else if (a[0] != '-' && !text) text = a;
        else { fprintf(stderr, "render spec: opcao desconhecida: %s\n", a); return 1; }
    }
    if (brief) {                                                              /* o briefing e o prompt: mostra o que o motor mandaria e "tokens N/limite", ou o erro de recusa (passou do limite), sem gerar nada (render/sdbrief.c) */
        char *bt = sdb_read_text(brief); if (!bt) return 1;
        if (!strcmp(fengine, "flux")) {
            char *pr = sdb_flux_prompt(bt, text, stdout); if (!pr) return 1;
            printf("--- FLUX (o briefing, sem reescrita) ---\nprompt: %s\n", pr);
        } else if (!strcmp(fengine, "stable")) {
            SdgFile base; SdClip clip; const SdClip *cp = NULL; char mp[1100];
            snprintf(mp, sizeof mp, "%s/render/sd15-realistic-vision.gguf", models_dir());
            if (sdg_open(&base, mp) && sdc_load(&clip, &base)) cp = &clip;
            char *pr = sdb_stable_prompt(bt, text, cp, chunks < 1 ? 1 : chunks > SDB_MAX_CHUNKS ? SDB_MAX_CHUNKS : chunks, stdout); if (!pr) return 1;
            printf("--- Stable Diffusion (o briefing, sem reescrita) ---\nprompt: %s\nnegative: %s\n", pr, SDB_NEG_DEFAULT);
        } else { fprintf(stderr, "render spec: --engine flux ou stable\n"); return 1; }
        return 0;
    }
    if (!text) { fprintf(stderr, "uso: katana render spec \"<pedido>\" [--ref ref.png] [--lcm] [--chunks 4] [--long 512] [--final [--final-engine flux|krea]] [--json] [--no-rag]\n"); return 1; }
    if (chunks < 1) chunks = 1; if (chunks > 16) chunks = 16;
    SdSpec s;
    sds_parse(&s, text, image != NULL, !norag);
    if (final_mode) s.final_mode = 1;
    SdgFile base; SdClip clip; const SdClip *cp = NULL; char path[1100];
    snprintf(path, sizeof path, "%s/render/sd15-realistic-vision.gguf", models_dir());
    if (sdg_open(&base, path) && sdc_load(&clip, &base)) cp = &clip;
    int iw = 0, ih = 0; if (image) sdp_peek_size(image, &iw, &ih);
    SdsPlan pl; sds_plan(&s, image != NULL, iw, ih, chunks, lcm, longside, cp, &pl);
    if (json) { sds_json(&s, stdout); return 0; }
    sds_json(&s, stdout);
    if (s.final_mode) {                                                       /* render final: outro motor, o preview aprovado como referencia */
        if (strcmp(fengine, "flux") && strcmp(fengine, "krea")) { fprintf(stderr, "render spec: --final-engine flux ou krea\n"); return 1; }
        char prose[3000]; sds_prose(&s, prose, sizeof prose);
        printf("--- render final (%s, a testar contra %s) ---\nreferencia: o preview aprovado (--ref), forca 0.40: a composicao ja aprovada nao muda\nprompt: %s\n", fengine, strcmp(fengine, "flux") ? "flux" : "krea", prose);
        if (!image) printf("aviso: sem --ref; o final parte de uma imagem aprovada (o preview); sem ela seria uma geracao nova\n");
        return 0;
    }
    printf("--- Stable Diffusion ---\nprompt (%d bloco(s) de 75): %s\nnegative: %s\nsize: %dx%d  steps: %d  guidance: %.1f%s  strength: %.2f\n", pl.chunks_used, pl.positive, pl.negative, pl.w, pl.h, pl.steps, pl.guidance, pl.lcm ? "  (LCM)" : "", pl.strength);
    if (!strcmp(s.intent, "inpaint")) printf("aviso: pedido de edicao localizada (inpaint); ainda sem mascara, segue como imagem a imagem\n");
    if (s.unknown[0]) printf("aviso: palavras sem traducao: %s. O CLIP so entende ingles: palavra fora do lexico segue como veio\n", s.unknown);
    return 0;
}

/* katana render generate --nl "<pedido>": preenche o prompt, o negativo e os parametros que o usuario nao deu (tamanho, guia, forca) */
static int sds_fill(SdpParams *P, const char *nl) {
    SdSpec s;
    sds_parse(&s, nl, P->init != NULL, 1);
    SdgFile base; SdClip clip; const SdClip *cp = NULL; char path[1100];
    snprintf(path, sizeof path, "%s/sd15-realistic-vision.gguf", P->models);
    if (sdg_open(&base, path) && sdc_load(&clip, &base)) cp = &clip;
    int iw = 0, ih = 0; if (P->init) sdp_peek_size(P->init, &iw, &ih);
    SdsPlan pl; sds_plan(&s, P->init != NULL, iw, ih, P->max_chunks, P->lcm, 576.0, cp, &pl);
    P->prompt = strdup(pl.positive); P->negative = strdup(pl.negative);
    if (!P->w && !P->h && !P->init) { P->w = pl.w; P->h = pl.h; }
    if (P->strength == 0.75 && P->init) P->strength = pl.strength;
    if (!P->lcm && P->guidance == 7.5) P->guidance = pl.guidance;
    if (!strcmp(s.intent, "inpaint")) fprintf(stderr, "[spec] pedido de edicao localizada (inpaint): ainda sem mascara, segue como imagem a imagem\n");
    if (s.unknown[0]) fprintf(stderr, "[spec] palavras sem traducao: %s (o CLIP so entende ingles)\n", s.unknown);
    fprintf(stderr, "[spec] dominio %s/%s; prompt (%d bloco(s)): %s\n[spec] negativo: %s\n", s.domain, s.scene, pl.chunks_used, pl.positive, pl.negative);
    return 1;
}

/* katana render generate --engine flux --nl "<pedido>": o mesmo pedido, em prosa para o FLUX (sem pesos, sem limite de 75 tokens e sem a clausula "Avoid":
 * o modelo e destilado, nao tem prompt negativo, e citar o que evitar o convida a aparecer) */
static void sds_nl_prose(const char *nl, int has_image, char *out, size_t cap) {
    SdSpec s;
    sds_parse(&s, nl, has_image, 1);
    s.negative[0] = 0;
    sds_prose(&s, out, cap);
    if (!out[0]) snprintf(out, cap, "%s", nl);                                 /* nada reconhecido: o pedido como veio */
    if (!strcmp(s.intent, "inpaint")) fprintf(stderr, "[spec] pedido de edicao localizada (inpaint): ainda sem mascara, segue como imagem a imagem\n");
    fprintf(stderr, "[spec] dominio %s/%s; prompt do FLUX: %s\n", s.domain, s.scene, out);
}
