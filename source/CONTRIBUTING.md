# Contributing to Wavy-Flo

Thank you for helping. Bug reports, ideas and code are all welcome.

- **Bugs:** open an issue saying what you did, what you expected and what
  happened. If Wavy-Flo crashed, attach the `crash-report*.txt` and
  `wavy-flo-log.txt` from beside `Wavy-Flo.exe`. A small project file that
  shows the problem helps most.
- **Code:** open a pull request with one change at a time.
  - Build it and run the self-tests first (see [BUILD.md](BUILD.md)). Every
    suite should pass.
  - Real-time playback on modest hardware is the first goal: measure a change
    that touches rendering, and say what you measured.
  - Follow the code around your change: its naming, its comments, its layout.
- **Licence:** by sending a contribution you agree that it is licensed under the
  GNU General Public License, version 3 or later, like the rest of Wavy-Flo
  (and under the MIT License if it is in `sdk/`). Send only work that is yours
  to give, or that carries a licence compatible with these.
- **Effects:** an effect pack can live in its own repository under any licence
  -- the SDK in `sdk/` is MIT for that reason.

## Credit

Everyone who helps is credited.

- **Code:** the commits in your pull request keep your name. GitHub lists you
  among the contributors on the repository's page and in
  [Insights > Contributors](https://github.com/Kibo4aa/Wavy-Flo-Releases/graphs/contributors),
  as long as your commits use an email address linked to your GitHub account.
  When several people made one change, add a line to the commit message for
  each of the others: `Co-authored-by: Name <email>`.
- **Everything else:** bug reports, testing, translations, demos, tutorials,
  art and effect packs. Your name goes in [CREDITS.md](CREDITS.md), under the
  heading that fits. Add it in your pull request, or ask in your issue and it
  will be added for you.
- **In Wavy-Flo:** CREDITS.md is built into the program and shown in
  **Help > About Wavy-Flo**, and each release's notes thank the people whose
  work is in it.
