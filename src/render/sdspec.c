/* ============================ render: pedido em linguagem natural -> especificacao de imagem -> prompt do Stable Diffusion ===========
 * katana render spec "<pedido>" [--ref ref.png] [--lcm] [--chunks 4] [--long 512] [--json]     mostra a especificacao e o que o SD recebe
 * katana render generate --nl "<pedido>" --out saida.png [--ref ref.png] ...                     gera a partir do pedido inteiro
 * katana render generate --spec spec.json --out saida.png                                          especificacao escrita pelo LLM do agente
 *
 * Quatro camadas independentes (docs/render.md, "UIIE"):
 *   1. Interpretacao   pedido (PT ou EN) -> SdSpec: assunto, ambiente, estilo, luz, composicao, camera, qualidade, negativo, proporcao,
 *                      finalidade. Tabelas de conceitos por palavra; a clausula que nenhuma palavra reconhece vai ao RAG (es_rank, embedding
 *                      multilingue) contra as descricoes dos conceitos, e so entra acima de um limiar com folga sobre o segundo. O que sobra
 *                      e conteudo (assunto/ambiente) e passa por um lexico PT->EN; palavra sem traducao fica registrada em unknown[] e
 *                      avisada (o CLIP so entende ingles: nesse caso o LLM do agente deve escrever a especificacao em JSON, --spec).
 *   2. Configuracao    codigo, nao texto: proporcao -> largura x altura (multiplos de 64), passos, guidance, forca da imagem de referencia.
 *                      O usuario nao pede "30 passos"; a politica do motor decide.
 *   3. Roteador        com --ref e imagem a imagem; sem ela, texto para imagem. "remova/troque o fundo" (inpaint) e reconhecido e avisado,
 *                      ainda sem mascara: segue como imagem a imagem.
 *   4. Adaptador SD    SdSpec + config -> prompt positivo (assunto com peso (x:1.2), ordem de importancia), negativo coerente com o estilo e
 *                      os campos de SdpParams; corta nos blocos de 75 tokens do CLIP. Outro modelo (FLUX, Krea) tera outro adaptador.
 * A especificacao e canonica (JSON), sem nada do SD dentro dela. */

typedef struct {
    char intent[16];                                    /* text_to_image | image_to_image | inpaint (so reconhecido) */
    char subject[240], environment[240], style[200], lighting[200], composition[200], camera[160], quality[200], extra[240], negative[300];
    char domain[24], scene[32];                          /* roteador: architecture, product, person, landscape, general; cena (exterior/interior) */
    char purpose[24];                                   /* wallpaper, profile, banner, ... ou vazio */
    char concept[200], mood[120], hierarchy[120], palette[160], materials[200];   /* direcao de arte (Visual Prompt Director) */
    int final_mode;                                     /* 0 = preview (composicao forte, rapido), 1 = final (mais passos e detalhe) */
    double aspect;                                      /* largura/altura pedida; 0 = nao pedida */
    int person;                                         /* o assunto tem uma pessoa (negativos de anatomia) */
    char unknown[200];                                  /* palavras do conteudo sem traducao */
    char *positive_override;                            /* positive_prompt do JSON do LLM, se veio */
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

/* lexico PT -> EN do conteudo (assunto e ambiente); radicais sem acento. "" = palavra funcional, descartada */
static const struct { const char *pt, *en; } SDS_LEX[] = {
    { "piscina", "swimming pool" }, { "fachada", "facade" }, { "pavimentos", "stories" }, { "pavimento", "story" }, { "dois", "two" }, { "tres", "three" }, { "tropical", "tropical" }, { "aparente", "exposed" },
    { "varanda", "balcony" }, { "telhado", "roof" }, { "garagem", "garage" }, { "paisagismo", "landscaping" }, { "vegetacao", "vegetation" }, { "arvores", "trees" }, { "arvore", "tree" }, { "flores", "flowers" },
    { "tenis", "sneaker" }, { "garrafa", "bottle" }, { "relogio", "watch" }, { "perfume", "perfume bottle" }, { "bolsa", "handbag" }, { "campanha", "" }, { "publicitaria", "" }, { "publicidade", "" }, { "produto", "product" },
    { "futurista", "futuristic" }, { "esportiva", "sports" }, { "premium", "premium" }, { "moderna", "modern" }, { "grandes", "large" }, { "portas", "doors" }, { "janelas", "windows" },
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
        if (kl <= 3 && sds_isw(p[kl])) continue;
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
            if (l <= 3 && sds_isw(q[l])) continue;
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

/* clausula sem palavra de conceito: o RAG compara com as descricoes dos conceitos; 1 = aplicou algum */
static int sds_rag(SdSpec *s, const char *clause) {
    static const char *chunk[SDS_NC]; static char desc[SDS_NC][220]; static int ready;
    int idx[SDS_NC], n = 0;
    for (int i = 0; i < SDS_NC; i++) {
        if (SDS_C[i].cat == SC_ASPECT || !SDS_C[i].phrase[0]) continue;
        if (!ready) snprintf(desc[i], sizeof desc[i], "%s (%s)", SDS_C[i].phrase, SDS_C[i].keys);
        chunk[n] = desc[i]; idx[n++] = i;
    }
    ready = 1;
    float sc[SDS_NC];
    if (!es_available() || es_rank(clause, EMB_SEARCH, chunk, NULL, n, 4000.0, sc) <= 0) return 0;
    int b1 = -1, b2 = -1;
    for (int i = 0; i < n; i++) { if (b1 < 0 || sc[i] > sc[b1]) { b2 = b1; b1 = i; } else if (b2 < 0 || sc[i] > sc[b2]) b2 = i; }
    const float lim = getenv("KATANA_SPEC_RAG") ? (float)atof(getenv("KATANA_SPEC_RAG")) : 0.62f;
    if (b1 >= 0 && sc[b1] >= lim && (b2 < 0 || sc[b1] - sc[b2] >= 0.03f)) { sds_apply(s, &SDS_C[idx[b1]]); return 1; }
    return 0;
}

static void sds_parse(SdSpec *s, const char *text, int has_image, int use_rag) {
    memset(s, 0, sizeof *s);
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

/* ---------- JSON: leitura (do LLM) e escrita ---------- */
static const char *sds_js(const JV *o, const char *a, const char *b) {          /* string em o.a.b (ou o.a se b NULL) */
    const JV *x = jget(o, a);
    if (x && b) x = jget(x, b);
    return x ? jstr(x, NULL) : NULL;
}
static void sds_set(char *dst, size_t cap, const char *v) { if (v) snprintf(dst, cap, "%s", v); }

/* le a especificacao de um JSON no formato do prompt de sistema (intent, subject, environment, style, lighting, composition, positive_prompt,
 * negative_prompt, requested_aspect_ratio); valida os tipos e devolve 0 com a mensagem em err se faltar o minimo */
static int sds_from_json(SdSpec *s, const char *path, char *err, size_t ecap) {
    JV j;
    if (!cv_json_file(path, &j) || j.t != J_OBJ) { snprintf(err, ecap, "%s nao e um JSON de objeto", path); return 0; }
    memset(s, 0, sizeof *s);
    const char *in = jstr(&j, "intent");
    snprintf(s->intent, sizeof s->intent, "%s", in && (!strcmp(in, "image_to_image") || !strcmp(in, "inpaint")) ? in : "text_to_image");
    const JV *sub = jget(&j, "subject");
    if (sub && sub->t == J_STR) sds_set(s->subject, sizeof s->subject, jstr(&j, "subject"));
    else if (sub) sds_set(s->subject, sizeof s->subject, sds_js(&j, "subject", "description"));
    const JV *env = jget(&j, "environment");
    if (env && env->t == J_STR) sds_set(s->environment, sizeof s->environment, jstr(&j, "environment"));
    else if (env) { sds_set(s->environment, sizeof s->environment, sds_js(&j, "environment", "location")); const char *bg = sds_js(&j, "environment", "background"); if (bg) sds_cat(s->environment, sizeof s->environment, bg); }
    const JV *st = jget(&j, "style");
    if (st && st->t == J_STR) sds_set(s->style, sizeof s->style, jstr(&j, "style")); else if (st) { sds_set(s->style, sizeof s->style, sds_js(&j, "style", "type")); const char *q = sds_js(&j, "style", "quality"); if (q) sds_cat(s->quality, sizeof s->quality, q); }
    const JV *li = jget(&j, "lighting");
    if (li && li->t == J_STR) sds_set(s->lighting, sizeof s->lighting, jstr(&j, "lighting")); else if (li) sds_set(s->lighting, sizeof s->lighting, sds_js(&j, "lighting", "type"));
    const JV *co = jget(&j, "composition");
    if (co && co->t == J_STR) sds_set(s->composition, sizeof s->composition, jstr(&j, "composition")); else if (co) { sds_set(s->composition, sizeof s->composition, sds_js(&j, "composition", "framing")); const char *a = sds_js(&j, "composition", "camera_angle"); if (a) sds_cat(s->composition, sizeof s->composition, a); }
    const JV *ad = jget(&j, "art_direction");
    if (ad && ad->t == J_OBJ) { sds_set(s->concept, sizeof s->concept, sds_js(&j, "art_direction", "concept")); sds_set(s->mood, sizeof s->mood, sds_js(&j, "art_direction", "mood")); sds_set(s->hierarchy, sizeof s->hierarchy, sds_js(&j, "art_direction", "visual_hierarchy")); }
    const JV *pa = jget(&j, "color_palette");
    if (pa && pa->t == J_ARR) for (size_t k = 0; k < pa->n && k < 6; k++) if (pa->v[k].t == J_STR) sds_cat(s->palette, sizeof s->palette, jstr(&pa->v[k], NULL) ? jstr(&pa->v[k], NULL) : "");
    const JV *sd = jget(&j, "subject");
    if (sd && sd->t == J_OBJ) { const JV *dt = jget(sd, "details"); if (dt && dt->t == J_ARR) for (size_t k = 0; k < dt->n && k < 6; k++) if (dt->v[k].t == J_STR) sds_cat(s->materials, sizeof s->materials, jstr(&dt->v[k], NULL) ? jstr(&dt->v[k], NULL) : ""); const char *pr = sds_js(&j, "subject", "primary"); if (pr) sds_set(s->subject, sizeof s->subject, pr); }
    const JV *pv = jget(&j, "preview");
    if (pv && pv->t == J_OBJ) { const char *ra = sds_js(&j, "preview", "aspect_ratio"); int a, b; if (ra && sscanf(ra, "%d:%d", &a, &b) == 2 && a > 0 && b > 0) s->aspect = (double)a / b; }
    const char *md = jstr(&j, "mode"); if (md && !strcmp(md, "final")) s->final_mode = 1;
    const char *ng = jstr(&j, "negative_prompt"); if (ng) sds_set(s->negative, sizeof s->negative, ng);
    const char *pp = jstr(&j, "positive_prompt"); if (pp) s->positive_override = strdup(pp);
    const JV *ar = jget(&j, "requested_aspect_ratio");
    if (ar && ar->t == J_STR) { int a, b; const char *v = jstr(&j, "requested_aspect_ratio"); if (v && sscanf(v, "%d:%d", &a, &b) == 2 && a > 0 && b > 0) s->aspect = (double)a / b; }
    if (!s->subject[0] && !s->positive_override) { snprintf(err, ecap, "o JSON nao tem subject nem positive_prompt; reescreva a especificacao (subject em ingles)"); return 0; }
    return 1;
}

static void sds_json(const SdSpec *s, FILE *f) {
    const struct { const char *k; const char *v; } F[] = { { "intent", s->intent }, { "domain", s->domain }, { "scene", s->scene }, { "subject", s->subject }, { "environment", s->environment }, { "style", s->style }, { "lighting", s->lighting },
        { "composition", s->composition }, { "camera", s->camera }, { "quality", s->quality }, { "negative", s->negative }, { "purpose", s->purpose },
        { "concept", s->concept }, { "mood", s->mood }, { "visual_hierarchy", s->hierarchy }, { "color_palette", s->palette }, { "details", s->materials } };
    fputs("{\n", f);
    for (int i = 0; i < 17; i++) { fprintf(f, "  \"%s\": \"", F[i].k); for (const char *p = F[i].v; *p; p++) { if (*p == '"' || *p == '\\') fputc('\\', f); fputc(*p, f); } fputs("\",\n", f); }
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
    if (s->positive_override) snprintf(pr, sizeof pr, "%s", s->positive_override);
    else {
        char subj[300]; if (s->subject[0]) snprintf(subj, sizeof subj, "(%s:1.2)", s->subject); else subj[0] = 0;
        char pal[200]; if (s->palette[0]) snprintf(pal, sizeof pal, "%s color palette", s->palette); else pal[0] = 0;
        const char *parts[] = { s->concept, s->style, subj, s->materials, s->environment, s->composition, s->lighting, s->mood, pal, s->camera, s->quality, s->extra };
        for (int i = 0; i < 12; i++) if (parts[i][0]) { char trial[2400]; snprintf(trial, sizeof trial, "%s%s%s", pr, pr[0] ? ", " : "", parts[i]); if (i == 2 || sdb_tokens(clip, trial) <= budget) snprintf(pr, sizeof pr, "%s", trial); }
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
    const struct { const char *pre, *v; } P[] = { { "", s->concept }, { "A ", s->style }, { "Subject: ", s->subject }, { "Details: ", s->materials }, { "Setting: ", s->environment }, { "Composition: ", s->composition },
        { "Lighting: ", s->lighting }, { "Mood: ", s->mood }, { "Color palette: ", s->palette }, { "Camera: ", s->camera }, { "", s->quality } };
    for (int i = 0; i < 11; i++) if (P[i].v[0]) { strncat(out, P[i].pre, cap - strlen(out) - 1); strncat(out, P[i].v, cap - strlen(out) - 1); strncat(out, ". ", cap - strlen(out) - 1); }
    if (s->negative[0]) { strncat(out, "Avoid: ", cap - strlen(out) - 1); strncat(out, s->negative, cap - strlen(out) - 1); strncat(out, ".", cap - strlen(out) - 1); }
}

static int sds_main(int argc, char **argv) {
    const char *text = NULL, *image = NULL, *specf = NULL; int chunks = 4, lcm = 0, json = 0, norag = 0, final_mode = 0; const char *fengine = "flux"; double longside = 512;
    for (int i = 0; i < argc; i++) {
        const char *a = argv[i]; const int more = i + 1 < argc;
        if (!strcmp(a, "--ref") && more) image = argv[++i];
        else if (!strcmp(a, "--spec") && more) specf = argv[++i];
        else if (!strcmp(a, "--chunks") && more) chunks = atoi(argv[++i]);
        else if (!strcmp(a, "--long") && more) longside = atof(argv[++i]);
        else if (!strcmp(a, "--lcm")) lcm = 1;
        else if (!strcmp(a, "--json")) json = 1;
        else if (!strcmp(a, "--no-rag")) norag = 1;
        else if (!strcmp(a, "--final")) final_mode = 1;
        else if (!strcmp(a, "--final-engine") && more) fengine = argv[++i];
        else if (a[0] != '-' && !text) text = a;
        else { fprintf(stderr, "render spec: opcao desconhecida: %s\n", a); return 1; }
    }
    if (!text && !specf) { fprintf(stderr, "uso: katana render spec \"<pedido>\" [--ref ref.png] [--lcm] [--chunks 4] [--long 512] [--final [--final-engine flux|krea]] [--json] [--no-rag]\n       katana render spec --spec spec.json\n"); return 1; }
    if (chunks < 1) chunks = 1; if (chunks > 16) chunks = 16;
    SdSpec s; char err[300];
    if (specf) { if (!sds_from_json(&s, specf, err, sizeof err)) { fprintf(stderr, "render spec: %s\n", err); return 1; } }
    else sds_parse(&s, text, image != NULL, !norag);
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
    if (s.unknown[0]) printf("aviso: palavras sem traducao: %s. O CLIP so entende ingles; peca ao LLM do agente uma especificacao em JSON (--spec)\n", s.unknown);
    return 0;
}

/* katana render generate --nl "<pedido>" | --spec spec.json: preenche o prompt, o negativo e os parametros que o usuario nao deu (tamanho, guia, forca) */
static int sds_fill(SdpParams *P, const char *nl, const char *specf) {
    SdSpec s; char err[300];
    if (specf) { if (!sds_from_json(&s, specf, err, sizeof err)) { fprintf(stderr, "render: %s\n", err); return 0; } }
    else sds_parse(&s, nl, P->init != NULL, 1);
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
    if (s.unknown[0]) fprintf(stderr, "[spec] palavras sem traducao: %s (o CLIP so entende ingles; use --spec com o JSON do LLM do agente)\n", s.unknown);
    fprintf(stderr, "[spec] dominio %s/%s; prompt (%d bloco(s)): %s\n[spec] negativo: %s\n", s.domain, s.scene, pl.chunks_used, pl.positive, pl.negative);
    return 1;
}
