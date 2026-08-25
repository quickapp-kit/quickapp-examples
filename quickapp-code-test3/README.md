# BLOCK-001 Runtime Contract Fixture

This fixture validates keyed Block lifecycle with real Toolkit output:

- `[A, B] -> [A, B, C]` creates only C.
- `[A, B, C] -> [A, C]` removes B and releases its handler and host objects.
- `[A, C] -> [A, B, C]` creates a new B identity.

Case 002 supplements Case 001 with behavior that Case 001 does not contain:

- click-driven state mutation;
- incremental text update;
- conditional Block create/remove;
- keyed list Block move/reuse.

It uses the alliance DSL and is a contract fixture, not a product demo.
