def handle(request):
    label = request.get("label")
    if not label:
        return {"status": "error", "reason": "missing label"}
    return {"status": "cancelled", "label": label}
