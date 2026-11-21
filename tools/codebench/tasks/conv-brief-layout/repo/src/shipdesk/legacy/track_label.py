from ..core.respond import ok


def handle(request):
    return ok(label=request.get("label"), where="depot")
