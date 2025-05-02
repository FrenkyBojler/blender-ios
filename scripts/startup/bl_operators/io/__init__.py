from bl_operators.io import curve_svg

io_operator_modules = (
    curve_svg,
)

classes = []

for io_operator in io_operator_modules:
    classes.extend(io_operator.classes)
