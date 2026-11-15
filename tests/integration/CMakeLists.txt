# Cross-module tests (design.md 14: golden audio, determinism, load/burst,
# feature coverage). They link the full `sonus` library plus the mocks.
snd_add_test(integration_test
  SOURCES integration_smoke_test.cpp
  LINK sonus sonus_mocks)
