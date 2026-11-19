from shop.pricing import apply_discount

def line_total(price, qty, pct=0):
    return price * qty

def total(items, discount_pct=0):
    subtotal = sum(line_total(price, qty) for _name, price, qty in items)
    return round(apply_discount(subtotal, discount_pct), 2)
