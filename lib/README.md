# lib: Wavert's standard library, written in Wavert

General helpers that any program can use, imported with the reserved prefix `$lib/`:

```
;import grid from "$lib/grid.wvt"
```

`wvc` finds this folder from where it is: `lib/` next to `wvc`, then one level up, so `wvc/wvc.exe` and `.wvt/wvc.exe` both find it. `--lib <dir>` names another folder. The current directory is not searched, so a program's own `lib/` folder never stands in for this one. See §13.1 of the [specification](../docs/wavert-spec.md).

These libraries change with the language and are tested with it (`wvc/test`), so they stay compatible with the compiler they ship with. Code for one kind of program, such as a roguelike's field of view, belongs with that program, not here.
