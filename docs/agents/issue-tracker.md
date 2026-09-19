# Issue tracker: GitHub

Issues and PRDs for this repository live in GitHub Issues at
`https://github.com/SW26010/spectiary`. Use the `gh` CLI from the repository
checkout for issue operations.

## Conventions

- Create: `gh issue create --title "..." --body "..."`.
- Read: `gh issue view <number> --comments`.
- List: `gh issue list` with explicit state and label filters as needed.
- Comment: `gh issue comment <number> --body "..."`.
- Edit labels: `gh issue edit <number> --add-label "..."` or
  `--remove-label "..."`.

When an engineering skill says to publish to the issue tracker, create a GitHub
issue. Do not close or modify a parent issue unless the user explicitly asks.
