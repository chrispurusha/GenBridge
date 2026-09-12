# gbParams.c notes

The longer comments from `gbParams.c`, moved here 2026-09-12 so the code reads cleanly. The code points at each as `// notes §k`. Verbatim and in file order; each is titled by what it documents.

## 1. in `gb_param_info()`

Every entry is automatable and every list is a list; what varies is the scale and the names.
A STEPPED LIST PARAMETER IS THE DEVICE CHOOSER, and a host renders one as a drop-down in its
generic panel - which makes the plug-in usable with no editor at all, and keeps working
afterwards because it is automatable and the host saves it. The step count is fixed at
registration and cached by the host, so it cannot track how many devices the machine happens
to have; unused slots simply read "-".
