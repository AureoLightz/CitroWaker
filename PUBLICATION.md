# Publication audit — November 13, 2026

Reviewed the main branch and its complete ten-commit development history before the publication-preparation commit. At that point there were 26 unique historical file blobs; the only three binary blobs were the documented PNG gameplay screenshots. The sole branch was main, and no release downloads were published.

No ISO, extracted game payload, generated material/texture pack, Nintendo music, CIA, save, crash dump, private filesystem path or credential pattern was found. This was a file/content/history review, not a legal clearance or an exhaustive secret-detection guarantee. The screenshots intentionally contain gameplay imagery; see RIGHTS.md.

Prepared for publication:

- Removed author/committer email addresses entirely, preserving historical file trees and commit messages.
- Added ignore rules for game data, generated assets, build products and private diagnostics.
- Documented repository scope and the absence of a blanket open-source licence.
- Corrected historical hardware explanations that had been stated too confidently, including the disproved unused-texture-unit hang theory.
- Verified included PNG metadata, local Markdown file links and all four overlay-tool tests.

This review covers this documentation/showcase-tools repository only. The separate local game source, renderer, packaging tree and new game-derived Master Sword presentation assets are not cleared for upload by this audit. Do not merge an entire local workspace into this repository.

Future publication of source, binaries or dependencies needs its own provenance/licence and asset review. Repository visibility remains the owner's decision. Rewritten commits may still exist in GitHub caches or previously made clones; changing the branch history does not guarantee erasure of old metadata everywhere.
