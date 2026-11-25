import math


class Shape:
    sides = 0

    def __init__(self, name):
        self.name = name

    def area(self):
        return 0.0

    def describe(self):
        return "%s with %d sides" % (self.name, self.sides)

    @property
    def label(self):
        return self.name.upper()


class Circle(Shape):
    def __init__(self, r):
        Shape.__init__(self, "circle")
        self.r = r

    def area(self):
        return math.pi * self.r ** 2

    def scale(self, k):
        self.r = self.r * k
        return self


class Square(Shape):
    sides = 4

    def __init__(self, s):
        Shape.__init__(self, "square")
        self.s = s

    def area(self):
        return self.s * self.s


def total_area(shapes):
    t = 0.0
    for s in shapes:
        t += s.area()
    return t


def biggest(shapes):
    best = None
    for s in shapes:
        if best is None or s.area() > best.area():
            best = s
    return best
