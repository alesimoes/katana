# statlib: small statistics helpers
test: python3 -m unittest discover -s tests
layout:
  src/statlib/  library code
  tests/  unit tests
  tools/  verification scripts
done: sh tools/verify.sh
