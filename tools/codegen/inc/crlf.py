def greet(name):
    msg = "hi " + name  # polite
    return msg


def shout(name):
    return greet(name).upper()
