"""
The string properties 'filepath', 'filename', 'directory' and a 'files'
collection are assigned when present in the operator.

If 'filter_glob' property is present in the operator and it's not empty,
it will be used as a file filter (example value: `*.zip;*.py;*.exe`).

If 'check_existing' property is present and set to `True`, then operator will warn
if the provided filepath already exists by highlighting the filename input field in red.
"""
