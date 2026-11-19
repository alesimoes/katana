import unittest
from mdhtml import render


class T(unittest.TestCase):
    def test_empty(self):
        self.assertEqual(render(""), "")
        self.assertEqual(render("  \n\n  "), "")

    def test_headings(self):
        self.assertEqual(render("# Title"), "<h1>Title</h1>")
        self.assertEqual(render("### Three"), "<h3>Three</h3>")
        self.assertEqual(render("###### Six"), "<h6>Six</h6>")
        self.assertEqual(render("#NoSpace"), "<p>#NoSpace</p>")

    def test_paragraphs(self):
        self.assertEqual(render("one\ntwo\n\nthree"), "<p>one two</p>\n<p>three</p>")
        self.assertEqual(render("  padded  \n  lines "), "<p>padded lines</p>")

    def test_lists(self):
        self.assertEqual(render("- a\n- b"), "<ul><li>a</li><li>b</li></ul>")
        self.assertEqual(render("1. x\n2. y\n3. z"), "<ol><li>x</li><li>y</li><li>z</li></ol>")
        self.assertEqual(render("- a\n\n- b"), "<ul><li>a</li></ul>\n<ul><li>b</li></ul>")
        self.assertEqual(render("10. ten"), "<ol><li>ten</li></ol>")

    def test_inline(self):
        self.assertEqual(render("a **b** c"), "<p>a <strong>b</strong> c</p>")
        self.assertEqual(render("a *b* c"), "<p>a <em>b</em> c</p>")
        self.assertEqual(render("**x** and *y*"), "<p><strong>x</strong> and <em>y</em></p>")
        self.assertEqual(render("see [docs](http://x.y/z)"), '<p>see <a href="http://x.y/z">docs</a></p>')
        self.assertEqual(render("use `a*b*c` here"), "<p>use <code>a*b*c</code> here</p>")
        self.assertEqual(render("- **b** item"), "<ul><li><strong>b</strong> item</li></ul>")
        self.assertEqual(render("## The *end*"), "<h2>The <em>end</em></h2>")

    def test_escape(self):
        self.assertEqual(render("1 < 2 & 3 > 2"), "<p>1 &lt; 2 &amp; 3 &gt; 2</p>")
        self.assertEqual(render("`<b>`"), "<p><code>&lt;b&gt;</code></p>")

    def test_code_block(self):
        self.assertEqual(render("```\nx = 1 < 2\n\n**no**\n```"), "<pre><code>x = 1 &lt; 2\n\n**no**</code></pre>")
        self.assertEqual(render("```python\n- not a list\n```"), "<pre><code>- not a list</code></pre>")

    def test_mixed(self):
        src = "# T\nintro line\n- a\n- b\ntext after\n\n```\ncode\n```\n1. one"
        exp = "<h1>T</h1>\n<p>intro line</p>\n<ul><li>a</li><li>b</li></ul>\n<p>text after</p>\n<pre><code>code</code></pre>\n<ol><li>one</li></ol>"
        self.assertEqual(render(src), exp)


if __name__ == "__main__":
    unittest.main()
