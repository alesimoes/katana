# shipdesk: parcel label request handlers
test: python3 -m unittest discover -s tests
layout:
  src/shipdesk/handlers/  current request handlers, one module per action, each exposing handle(request)
  src/shipdesk/legacy/  frozen old handlers, kept for the v1 clients
  src/shipdesk/core/  shared helpers
  tests/  unit tests
rules:
  - New handlers go in src/shipdesk/handlers/, named after the action in snake_case.
  - Never add or edit modules in src/shipdesk/legacy/.
