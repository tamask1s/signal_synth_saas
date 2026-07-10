# Stable Codex approval workflows

The execution environment identifies approvals from the command prefix. Ad-hoc
heredocs such as `python3 - <<...` change on every use, so they repeatedly
request approval. The three wrappers below expose fixed, argument-validated
workflows instead. They never accept an arbitrary command or source code.

| Wrapper | Scope | Examples |
|---|---|---|
| `scripts/task1_tipusp_dolgok.py` | read/test/verification | `status`, `quality`, `live-verify`, `mail-status` |
| `scripts/task2_release_dolgok.py` | release changes | `deploy`, `commit`, `push` |
| `scripts/task3_mail_dolgok.py` | mail operations | `local-status`, `local-install`, `gmail-config`, `gmail-verify` |

For a persistent approval, approve the **wrapper command prefix**, for example
`scripts/task1_tipusp_dolgok.py`, instead of an unrestricted `python3` prefix.
The first invocation can still prompt: approval behavior is controlled by the
host product and cannot safely be bypassed from repository code. Subsequent
calls with the same wrapper prefix should reuse that rule.

Examples:

```sh
scripts/task1_tipusp_dolgok.py quality
scripts/task2_release_dolgok.py deploy
scripts/task3_mail_dolgok.py local-status
sudo scripts/task3_mail_dolgok.py gmail-config your-address@gmail.com
```

`gmail-config` intentionally prompts for a Google App Password on the terminal.
Do not pass it as a command-line argument or commit it to the repository.
