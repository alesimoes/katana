from shop.pricing import apply_discount

def line_total(price, qty, pct):
    return apply_discount(price * qty, pct)

def total(items, discount_pct=0):
    subtotal = sum(line_total(price, qty, discount_pct) for _name, price, qty in items)
    return round(apply_discount(subtotal, discount_pct), 2)
