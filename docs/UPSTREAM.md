# Upstream Mesa / Terakan

Terakan is developed by **Vitaliy Triang3l Kuzmin** in the main Mesa tree:

- GitLab: https://gitlab.freedesktop.org/Triang3l/mesa
- Branch used here: `Terakan_Backup_2026-06-10_2_Meta_MSAA`

This GitHub repository is a **patch queue + packaging** for AI-assisted development. Changes should eventually be contributed back upstream or kept as a maintained fork branch.

To sync with upstream:

```bash
cd mesa
git fetch origin Terakan_Backup_2026-06-10_2_Meta_MSAA
git checkout Terakan_Backup_2026-06-10_2_Meta_MSAA
git pull
# Re-apply patches from mesa-terakan-ai-development
```

When upstream merges our changes, drop the corresponding patch files from `patches/`.
