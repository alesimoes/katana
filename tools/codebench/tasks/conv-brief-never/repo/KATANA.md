# fixbook: product catalog loader
test: python3 -m unittest discover -s tests
layout:
  data/fixtures/  catalog fixtures used by the tests
  tools/  seed data and maintenance scripts
rules:
  - Fixtures in data/fixtures/ are edited by hand.
never:
  - regenerate the fixtures with the generator `*tools/regen.sh*`
done: the test command passes
