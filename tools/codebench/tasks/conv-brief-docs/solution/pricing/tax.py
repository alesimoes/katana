from .config import DEFAULT_RATE


def net(cents, discount_bp=0):
    return cents - (cents * discount_bp + 5000) // 10000


def add_tax(cents):
    return cents + (cents * DEFAULT_RATE + 5000) // 10000
