import unittest
from decimal import Decimal
from bank import Account, InsufficientFunds
class T(unittest.TestCase):
    def test_exact(self):
        a = Account()
        a.deposit(0.1); a.deposit(0.2)
        self.assertEqual(a.balance, Decimal("0.3"))
    def test_withdraw_all(self):
        a = Account()
        a.deposit(10)
        a.withdraw(10)
        self.assertEqual(a.balance, Decimal("0"))
    def test_overdraft(self):
        a = Account()
        a.deposit(5)
        with self.assertRaises(InsufficientFunds):
            a.withdraw(5.01)
        self.assertEqual(a.balance, Decimal("5"))
    def test_bad_amounts(self):
        a = Account()
        for bad in (0, -1):
            with self.assertRaises(ValueError):
                a.deposit(bad)
            with self.assertRaises(ValueError):
                a.withdraw(bad)
if __name__ == "__main__":
    unittest.main()
