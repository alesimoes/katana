# Units and rounding

- Every amount of money is an integer number of cents. Never use floats for money.
- Rates (tax, discounts) are integers in basis points: 1 bp = 0.01%, so a rate of 725 means 7.25%.
- Rounding is half up (0.5 always goes up), done with integer arithmetic.
