# cartcalc: tiny shopping cart total library
test: sh tools/check.sh --suite fast
lint: python3 -m py_compile cartcalc/cart.py
layout:
  cartcalc/  library code
  tests/  unit tests (test_fast_* are the quick ones)
  tools/  helper scripts
rules:
  - Run the test command above after every change to cartcalc/.
done: the test command passes after the last edit
