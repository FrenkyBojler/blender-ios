.. _info_contributing:

*****************************
Contribute Documentation
*****************************

This guide covers how to contribute to Blender's Python API documentation,
including writing examples, formatting documentation, and building the docs locally.


Setting Up Your Environment
============================

Prerequisites
-------------

Before you can build the documentation, you need:

1. **Blender Source Code**: Clone the Blender repository following the
   `official build instructions <https://developer.blender.org/docs/handbook/building_blender/>`__.

2. **Python Environment**: Set up a Python `virtual environment <https://docs.python.org/3/library/venv.html>`__.


Installing Documentation Requirements
--------------------------------------

The documentation build system requires several Python packages. These are listed
in ``doc/python_api/requirements.txt``.

Assuming you have activated your virtual environment, install the requirements with:

.. code-block:: bash
    
   # Install the requirements
   pip install -r doc/python_api/requirements.txt


Building the Documentation
---------------------------

Once you have the requirements installed, you can build the documentation:

.. code-block:: bash

   # From the Blender source root
   make doc_py

You can then open ``doc/python_api/sphinx-out/index.html`` in your browser.

Modifying API Documentation
===========================


API documentation is automatically generated from Blender's source code, meaning that 
class descriptions, method signatures, etc, are defined either in the Python C/API in 
C/C++ files or as standard  docstrings within Python files.

**To modify API class or method descriptions:**

1. Locate the relevant source file in the Blender repository.
2. Find the the relevant docstring either inside ``PyDoc_STRVAR(...)`` for the Python C/API or as standard docstring in a Python file.
3. Edit using **reStructuredText** formatting.
4. Rebuild the Python API docs with ``make doc_py`` to regenerate the pages.

Adding Example Code Snippets
============================
Code examples are a crucial part of the API documentation. They help users
understand how to use various classes, functions, and modules. They appear 
above the API reference for each class or module.


Example File Naming Convention
-------------------------------

Example files are located in ``doc/python_api/examples/`` and follow a specific
naming convention:

**Format**: ``module.ClassName.N.py``

Where:

- ``module`` is the full module path (e.g., ``bpy.types``, ``bpy.ops``, ``bmesh``)
- ``ClassName`` is the class or function name
- ``N`` is a number (1, 2, 3, etc.)

**Examples**:

.. code-block:: text

   bpy.app.timers.1.py            # First explainer text followed by code example.
   bpy.app.timers.2.py            # Second explainer text followed by code example.

The documentation system automatically discovers these files during the build process
and includes them in the appropriate class or module documentation pages.


Example File Structure
----------------------

Each example file should follow this structure:

.. code-block:: python

   """
   Example Title
   +++++++++++++

   A description of what this example demonstrates.
   This doc-string appears before the code in the documentation.

   You can use **reStructuredText** formatting here, for example:

   - *Italic* text with single asterisks
   - **Bold** text with double asterisks
   - ``inline code`` with double backticks
   - :class:`bpy.types.Operator` to link to API classes
   - Links to `external resources <https://www.blender.org/>`__
       
   .. note::

      You can use this to highlight important information.

   Everything after this doc-string is treated as a code block:
   """
   import bpy

   # Example code goes here
   print("This is an example")


**Important Notes**:

- The doc-string should be at the very beginning of the file.
- The doc-string should be enclosed in triple quotes ``"""``.
- Use section header underlines with ``+`` characters for the title.
- Everything after the doc-string is included as a code block in the documentation.
- To add additional code blocks with text in between add new files.

The doc-string at the top of your ``module.ClassName.N.py`` file is expected to follow 
`reStructuredText format <https://www.sphinx-doc.org/en/master/usage/restructuredtext/index.html>`__ 
so it can be rendered as plain text at the top of your file.


Best Practices for Documentation
=================================

Writing Good Examples
---------------------

1. **Keep it simple**: Focus on demonstrating one concept at a time.
2. **Make it runnable**: Examples should work when pasted into Blender's Python console or text editor.
3. **Use comments**: Explain what the code does, especially non-obvious parts.
4. **Follow conventions**: Use standard Blender API patterns and naming conventions.
5. **Include context**: Explain when and why someone would use this API.


Testing Your Changes
====================

After adding or modifying documentation:

1. **Rebuild the docs**: Run ``make doc_py`` 
2. **Check for warnings**: The build process will show warnings about broken links,
   missing references, or formatting issues
3. **Preview in browser**: Open the generated HTML files to see how they look


Contributing Your Changes
==========================

Once you've added or improved documentation: 

- **Create a pull request**: Follow Blender's `contribution guidelines <https://developer.blender.org/docs/handbook/contributing/>`__



Thank you for contributing to Blender's API documentation!
