from ..core.respond import ok


def handle(request):
    return ok(pong=True)
