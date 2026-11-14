# Security

SnowFall runs without administrator rights and never reads other applications' window
contents. Its only network use is the optional update check (off until the user agrees): one
GET a day of the GitHub "latest release" API with a `SnowFall/<version>` User-Agent and nothing
else, plus the downloads of an update the user starts. An update is installed only if its
`SHA256SUMS` carries a valid ECDSA P-256 signature of the release key (public key in
`core/update/update.cpp`) and the installer matches it; downloads are limited to this
repository's GitHub URLs. If you find a security or privacy problem, please report it
privately to the author via [Telegram](https://t.me/dave_cli) rather than in a public issue.
