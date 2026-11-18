.pragma library
// Colorizador de codigo simples (varias linguagens): devolve HTML para TextEdit RichText
var KW = {};
("if else elif for while do switch case break continue return goto default in is not and or as with from import export "
 + "def class struct enum union typedef static const extern inline volatile register sizeof new delete this self super "
 + "function let var async await yield try catch except finally throw raise lambda pass fn pub mut impl use mod match "
 + "true false null none nil undefined void public private protected namespace template typename using package interface "
 + "echo fi then done esac local readonly").split(" ").forEach(function (w) { KW[w] = 1; });
var TY = {};
("int char long short float double unsigned signed bool size_t uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t "
 + "string str list dict set tuple object String Integer Boolean u8 u16 u32 u64 i32 i64 f32 f64 usize").split(" ").forEach(function (w) { TY[w] = 1; });

function esc(s) { return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;"); }
function span(c, s) { return '<span style="color:' + c + '">' + esc(s) + '</span>'; }

function html(code, lang, c) {
    var hashCom = !/^(c|cpp|c\+\+|h|hpp|cs|objc)$/i.test(lang || "");
    var re = /(\/\/[^\n]*|\/\*[\s\S]*?\*\/|#[^\n]*)|("(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'|`[^`]*`)|(\b0x[0-9a-fA-F]+\b|\b\d[\d_.]*(?:e[+-]?\d+)?[a-zA-Z]*\b)|([A-Za-z_][A-Za-z_0-9]*)/g;
    var out = "", last = 0, m;
    while ((m = re.exec(code)) !== null) {
        out += esc(code.slice(last, m.index));
        last = re.lastIndex;
        if (m[1] !== undefined) {
            if (m[1][0] === "#" && !hashCom) out += span(c.kw, m[1]); else out += span(c.com, m[1]);
        } else if (m[2] !== undefined) out += span(c.str, m[2]);
        else if (m[3] !== undefined) out += span(c.num, m[3]);
        else {
            var w = m[4];
            if (KW[w]) out += span(c.kw, w);
            else if (TY[w]) out += span(c.ty, w);
            else if (code[last] === "(") out += span(c.fn, w);
            else out += esc(w);
        }
    }
    out += esc(code.slice(last));
    return '<pre style="margin:0; white-space:pre">' + out + "</pre>";
}
