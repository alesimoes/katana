from decimal import Decimal

class InsufficientFunds(Exception):
    pass

class Account:
    def __init__(self):
        self.balance = Decimal("0")

    def deposit(self, amount):
        amount = Decimal(str(amount))
        if amount <= 0:
            raise ValueError("amount must be positive")
        self.balance += amount

    def withdraw(self, amount):
        amount = Decimal(str(amount))
        if amount <= 0:
            raise ValueError("amount must be positive")
        if amount > self.balance:
            raise InsufficientFunds()
        self.balance -= amount
