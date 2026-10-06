# STGE language support

Syntax highlighting for `.stge` files in VS Code and Cursor.

## Install (Cursor)

From this repo:

```bash
# Local / non-remote Cursor
mkdir -p ~/.cursor/extensions
ln -sfn "$(pwd)/editors/vscode" ~/.cursor/extensions/stge.stge-language-0.0.2

# Cursor Remote (WSL): extensions load from cursor-server
mkdir -p ~/.cursor-server/extensions
ln -sfn "$(pwd)/editors/vscode" ~/.cursor-server/extensions/stge.stge-language-0.0.2
```

Reload the window (`Developer: Reload Window`) after install or after pulling grammar changes. Open a `.stge` file and check the language mode in the status bar — it should say **STGE** (click it and pick STGE if it still says Plain Text). There is no separate enable toggle.

## Install (VS Code)

Same idea with the VS Code extensions folder:

```bash
mkdir -p ~/.vscode/extensions
ln -sfn "$(pwd)/editors/vscode" ~/.vscode/extensions/stge.stge-language-0.0.2
```

Then reload VS Code.
