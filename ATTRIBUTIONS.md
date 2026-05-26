# Third-Party Attributions

This file is generated from `cmake/attributions.cmake`.
It inventories the third-party components redistributed in the `pristine-engine` install tree.

| Component | Version | Relationship | License | Owner | Upstream |
| --- | --- | --- | --- | --- | --- |
| slang | v11.0 | direct dependency | MIT | Mike Popoloski and slang contributors | https://github.com/MikePopoloski/slang/tree/v11.0 |
| fmt | 12.1.0 | transitive dependency via slang | MIT with fmt embedded-code exception | Victor Zverovich and fmt contributors | https://github.com/fmtlib/fmt/tree/12.1.0 |
| nlohmann/json | v3.11.3 | direct dependency | MIT | Niels Lohmann | https://github.com/nlohmann/json/tree/v3.11.3 |
| boost_unordered vendored header | vendored in slang v11.0 | transitive vendored header via slang | BSL-1.0 | Boost contributors listed in slang/external/boost_unordered.hpp | https://github.com/MikePopoloski/slang/blob/v11.0/external/boost_unordered.hpp |
