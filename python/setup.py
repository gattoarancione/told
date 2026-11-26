# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Claudio Russo (gattoarancione)

"""Present only to force a platform-tagged wheel.

Metadata lives in pyproject.toml. But the package ships a compiled shared library, so a
default build would tag the wheel `py3-none-any` and pip would happily install a macOS
dylib on Linux. Declaring ext modules makes setuptools tag the wheel for the platform it
was built on, which is the truth.
"""

from setuptools import setup
from setuptools.dist import Distribution


class BinaryDistribution(Distribution):
    def has_ext_modules(self) -> bool:  # noqa: D102
        return True

    def is_pure(self) -> bool:  # noqa: D102
        return False


setup(distclass=BinaryDistribution)
