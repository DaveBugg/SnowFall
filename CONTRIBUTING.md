# Contributing to SnowFall

Thanks for your interest! Bug reports, ideas and pull requests are welcome.

## Before you start

- For anything bigger than a small fix, open an issue first so we can agree on the approach.
- Read [docs/development.md](docs/development.md) (build, layout, rules) and
  [docs/architecture.md](docs/architecture.md).

## Ground rules (they are product requirements, not style preferences)

- No network access, no telemetry.
- Never read other applications' window contents, titles or pixels (tests may read pixels only
  inside their own test windows). Never log personal data.
- No drivers, DLL injection, services or elevation.
- Original code and art only — every sprite and icon is generated in code. Do not copy code or
  assets from other products.
- `core/` must stay free of OS headers; platform code goes to `platform/<os>/`.
- Code builds warning-free (`/W4` on MSVC) and the test suites pass.

## Contributor license grant

SnowFall is published under the PolyForm Noncommercial License 1.0.0, and the author also
offers separate commercial licenses. To keep that possible, contributions are accepted only
under the following terms. By submitting a pull request you confirm that:

1. the contribution is your original work (or you have the right to submit it), and
2. you grant DaveBugg (the project author) and recipients of software distributed by him a
   perpetual, worldwide, non-exclusive, royalty-free, irrevocable license to use, reproduce,
   modify, sublicense and distribute your contribution under the project's license **and under
   any other license terms, including commercial ones**, and
3. you keep the copyright in your contribution.

If you can't agree to this, please don't submit code — an issue describing the change is still
very welcome.

## Pull requests

- One topic per PR, with a short description of what and why.
- Add or update tests for behaviour changes.
- Keep user-visible strings in both Russian and English (`platform/windows/app/ui/strings_table.inc`).
