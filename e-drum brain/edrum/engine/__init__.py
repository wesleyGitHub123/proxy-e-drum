"""Pure engine (brain spec §2): records in, results out.

Nothing in this package may import from ``edrum.io`` or ``edrum.cli``, perform
I/O beyond explicitly-passed paths (profiles), print, or hold mutable global
state. This purity is load-bearing: it is what makes golden-fixture regression
nearly free (spec §10) and a future on-device port mechanical (spec §2).
"""
