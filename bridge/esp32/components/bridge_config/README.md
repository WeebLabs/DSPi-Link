# bridge_config

One header, `bridge_config.h`, holding every tunable number in the bridge:
timeouts, limits, queue depths and retry counts. Each constant has its unit in
its name and a comment giving its reason and spec section. Code elsewhere
uses these names and never repeats a number.

Portable: no ESP-IDF headers. No sources and no tests of its own.
