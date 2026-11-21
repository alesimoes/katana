from .config import DEFAULT_RATE


def net(cents, discount_bp=0):
    return cents - (cents * discount_bp + 5000) // 10000
