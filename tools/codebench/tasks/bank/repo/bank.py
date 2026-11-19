class InsufficientFunds(Exception):
    pass

class Account:
    def __init__(self):
        self.balance = 0.0

    def deposit(self, amount):
        self.balance += amount

    def withdraw(self, amount):
        if amount >= self.balance:
            raise InsufficientFunds()
        self.balance -= amount
