# SPDX-License-Identifier: GPL-3.0-or-later
"""Tests for KIAUH extension discovery and integration.

These tests simulate KIAUH's discover_extensions() mechanism to ensure
our extension code is discoverable without crashing KIAUH. This catches
the class of bug where KIAUH crashes with IndexError because extension
code doesn't match KIAUH's discovery conventions.
"""
import importlib
import inspect
import json
import sys
import types
import unittest
from abc import ABC, abstractmethod
from pathlib import Path
from unittest.mock import MagicMock, patch

# Path to scripts/kiauh/helixscreen/ relative to this test file
EXTENSION_DIR = Path(__file__).resolve().parent.parent / "helixscreen"
METADATA_PATH = EXTENSION_DIR / "metadata.json"

# Required fields in metadata.json per KIAUH convention
REQUIRED_METADATA_FIELDS = {
    "index",
    "module",
    "display_name",
    "description",
    "maintained_by",
}


class TestMetadataJson(unittest.TestCase):
    """Tests for metadata.json validity."""

    def test_metadata_json_valid(self):
        """metadata.json exists, is valid JSON, and has all required KIAUH fields."""
        self.assertTrue(METADATA_PATH.exists(), f"metadata.json not found at {METADATA_PATH}")

        with open(METADATA_PATH) as f:
            data = json.load(f)

        self.assertIn("metadata", data, "metadata.json must have a top-level 'metadata' key")

        metadata = data["metadata"]
        for field in REQUIRED_METADATA_FIELDS:
            self.assertIn(field, metadata, f"metadata.json missing required field: {field}")

    def test_metadata_module_matches_file(self):
        """The 'module' field in metadata.json points to an actual .py file."""
        with open(METADATA_PATH) as f:
            data = json.load(f)

        module_name = data["metadata"]["module"]
        module_file = EXTENSION_DIR / f"{module_name}.py"
        self.assertTrue(
            module_file.exists(),
            f"metadata.json 'module' field '{module_name}' doesn't match any file "
            f"(expected {module_file})",
        )


class MockBaseExtension(ABC):
    """Mock of KIAUH's BaseExtension ABC.

    This mirrors the interface that KIAUH expects extensions to implement.
    KIAUH's discover_extensions() uses inspect to find subclasses of this.
    """

    @abstractmethod
    def install_extension(self, **kwargs) -> None: ...

    @abstractmethod
    def remove_extension(self, **kwargs) -> None: ...


class MockLogger:
    """Mock of KIAUH's core.logger.Logger with static methods."""

    @staticmethod
    def print_status(msg): pass

    @staticmethod
    def print_ok(msg): pass

    @staticmethod
    def print_error(msg): pass

    @staticmethod
    def print_warn(msg): pass

    @staticmethod
    def print_info(msg): pass


def _setup_kiauh_mocks():
    """Set up sys.modules mocks to simulate the KIAUH import environment.

    Returns a dict of module names to mock modules for cleanup.
    """
    mocks = {}

    # Mock 'extensions' package
    extensions_pkg = types.ModuleType("extensions")
    extensions_pkg.__path__ = []
    mocks["extensions"] = extensions_pkg

    # Mock 'extensions.base_extension' with our mock ABC
    base_ext_mod = types.ModuleType("extensions.base_extension")
    base_ext_mod.BaseExtension = MockBaseExtension
    mocks["extensions.base_extension"] = base_ext_mod

    # Mock 'extensions.helixscreen' — this is our __init__.py
    # We load it for real, but we need the parent package in sys.modules first
    helixscreen_pkg = types.ModuleType("extensions.helixscreen")
    # Load actual constants from our __init__.py
    init_path = EXTENSION_DIR / "__init__.py"
    spec = importlib.util.spec_from_file_location("extensions.helixscreen", init_path)
    real_module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(real_module)
    # Copy attributes
    for attr in dir(real_module):
        if not attr.startswith("__"):
            setattr(helixscreen_pkg, attr, getattr(real_module, attr))
    mocks["extensions.helixscreen"] = helixscreen_pkg

    # Mock 'core' and 'core.logger'
    core_pkg = types.ModuleType("core")
    core_pkg.__path__ = []
    mocks["core"] = core_pkg

    logger_mod = types.ModuleType("core.logger")
    logger_mod.Logger = MockLogger
    mocks["core.logger"] = logger_mod

    # Mock 'utils' and 'utils.input_utils'
    utils_pkg = types.ModuleType("utils")
    utils_pkg.__path__ = []
    mocks["utils"] = utils_pkg

    input_utils_mod = types.ModuleType("utils.input_utils")
    input_utils_mod.get_confirm = MagicMock(return_value=False)
    mocks["utils.input_utils"] = input_utils_mod

    return mocks


class TestKiauhDiscovery(unittest.TestCase):
    """Tests simulating KIAUH's extension discovery mechanism."""

    def setUp(self):
        """Install mock modules into sys.modules."""
        self.mocks = _setup_kiauh_mocks()
        self.original_modules = {}
        for name, mod in self.mocks.items():
            self.original_modules[name] = sys.modules.get(name)
            sys.modules[name] = mod

    def tearDown(self):
        """Restore sys.modules to original state."""
        for name, original in self.original_modules.items():
            if original is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = original
        # Also clean up the extension module itself if loaded
        ext_module_name = "extensions.helixscreen.helixscreen_extension"
        sys.modules.pop(ext_module_name, None)

    def test_kiauh_discovery_simulation(self):
        """Simulate KIAUH's discover_extensions() and verify it finds our class.

        This is THE critical test. KIAUH does:
          1. importlib.import_module(f"extensions.{ext_name}.{module_name}")
          2. inspect.getmembers(module, predicate) where predicate checks
             isclass + issubclass(cls, BaseExtension) + cls is not BaseExtension
          3. Expects at least one result — IndexError if empty
        """
        # Step 1: Import the module the way KIAUH does
        ext_module_path = EXTENSION_DIR / "helixscreen_extension.py"
        spec = importlib.util.spec_from_file_location(
            "extensions.helixscreen.helixscreen_extension", ext_module_path
        )
        module = importlib.util.module_from_spec(spec)
        sys.modules["extensions.helixscreen.helixscreen_extension"] = module
        spec.loader.exec_module(module)

        # Step 2: Run KIAUH's exact discovery predicate
        def kiauh_predicate(member):
            return (
                inspect.isclass(member)
                and issubclass(member, MockBaseExtension)
                and member is not MockBaseExtension
            )

        members = inspect.getmembers(module, kiauh_predicate)

        # Step 3: This is where KIAUH does members[0] — empty list = IndexError crash
        self.assertTrue(
            len(members) > 0,
            "KIAUH discovery found NO extension classes! "
            "This would cause IndexError: list index out of range",
        )

        # Verify it found our specific class
        class_names = [name for name, cls in members]
        self.assertIn(
            "HelixscreenExtension",
            class_names,
            f"Expected HelixscreenExtension but found: {class_names}",
        )

    def test_extension_has_required_methods(self):
        """The discovered class has install, update, and remove methods."""
        ext_module_path = EXTENSION_DIR / "helixscreen_extension.py"
        spec = importlib.util.spec_from_file_location(
            "extensions.helixscreen.helixscreen_extension", ext_module_path
        )
        module = importlib.util.module_from_spec(spec)
        sys.modules["extensions.helixscreen.helixscreen_extension"] = module
        spec.loader.exec_module(module)

        cls = module.HelixscreenExtension
        for method_name in ("install_extension", "update_extension", "remove_extension"):
            self.assertTrue(
                hasattr(cls, method_name),
                f"HelixscreenExtension missing required method: {method_name}",
            )
            self.assertTrue(
                callable(getattr(cls, method_name)),
                f"{method_name} is not callable",
            )


class TestFindInstallDir(unittest.TestCase):
    """Tests for _find_install_dir() filesystem scanning."""

    def setUp(self):
        """Set up KIAUH mocks for importing."""
        self.mocks = _setup_kiauh_mocks()
        self.original_modules = {}
        for name, mod in self.mocks.items():
            self.original_modules[name] = sys.modules.get(name)
            sys.modules[name] = mod

        # Import the module
        ext_module_path = EXTENSION_DIR / "helixscreen_extension.py"
        spec = importlib.util.spec_from_file_location(
            "extensions.helixscreen.helixscreen_extension", ext_module_path
        )
        self.module = importlib.util.module_from_spec(spec)
        sys.modules["extensions.helixscreen.helixscreen_extension"] = self.module
        spec.loader.exec_module(self.module)

    def tearDown(self):
        """Restore sys.modules."""
        for name, original in self.original_modules.items():
            if original is None:
                sys.modules.pop(name, None)
            else:
                sys.modules[name] = original
        sys.modules.pop("extensions.helixscreen.helixscreen_extension", None)

    @patch("pathlib.Path.exists", return_value=False)
    @patch("pathlib.Path.is_dir", return_value=False)
    def test_find_install_dir_no_install(self, mock_is_dir, mock_exists):
        """_find_install_dir() returns None when no install exists."""
        result = self.module._find_install_dir()
        self.assertIsNone(result)

    def test_find_install_dir_found(self):
        """_find_install_dir() returns correct path when install exists."""
        # Mock Path.exists to simulate an install at ~/helixscreen with the
        # standard release layout (binary under bin/).
        expected_path = Path.home() / "helixscreen"

        def fake_exists(self_path):
            # The function checks path.exists() and _has_helix_screen(path),
            # which probes <path>/bin/helix-screen (preferred) or <path>/helix-screen.
            return str(self_path) in (
                str(expected_path),
                str(expected_path / "bin" / "helix-screen"),
            )

        with patch.object(Path, "exists", fake_exists):
            with patch.object(Path, "is_dir", return_value=False):
                result = self.module._find_install_dir()

        self.assertEqual(result, expected_path)

    def test_find_install_dir_legacy_layout(self):
        """_find_install_dir() also accepts pre-1.0 layout with binary at top level."""
        expected_path = Path.home() / "helixscreen"

        def fake_exists(self_path):
            # Legacy: only <path>/helix-screen exists, no bin/ subdir.
            return str(self_path) in (
                str(expected_path),
                str(expected_path / "helix-screen"),
            )

        with patch.object(Path, "exists", fake_exists):
            with patch.object(Path, "is_dir", return_value=False):
                result = self.module._find_install_dir()

        self.assertEqual(result, expected_path)


class TestInitNoSideEffects(unittest.TestCase):
    """Test that __init__.py import doesn't cause side effects."""

    def test_init_no_side_effects(self):
        """Importing __init__.py doesn't execute filesystem scanning or other side effects.

        The original bug was caused by __init__.py doing work at import time
        that could fail and crash KIAUH's discovery. Our __init__.py should
        only define constants.
        """
        init_path = EXTENSION_DIR / "__init__.py"
        spec = importlib.util.spec_from_file_location(
            "_test_helixscreen_init", init_path
        )
        module = importlib.util.module_from_spec(spec)

        # If import triggers filesystem scanning, subprocess calls, or network
        # access, it would fail or raise here since we haven't mocked anything
        # (except that Path constants are fine — they don't touch the filesystem)
        try:
            spec.loader.exec_module(module)
        except Exception as e:
            self.fail(f"__init__.py import caused side effect that raised: {e}")

        # The original bug was code at *module scope* that touched the
        # filesystem at import time. Defining a helper function is fine —
        # KIAUH only invokes it on user action. Skip re-exported imports
        # (typing.Optional is callable on Python 3.12+) and our intentional
        # find_install_dir helper.
        ALLOWED_DEFINED = {"find_install_dir"}
        public_attrs = [a for a in dir(module) if not a.startswith("_")]
        for attr in public_attrs:
            obj = getattr(module, attr)
            from_this_module = getattr(obj, "__module__", None) == module.__name__
            if not from_this_module or attr in ALLOWED_DEFINED:
                continue
            self.assertFalse(
                callable(obj) and not isinstance(obj, type) and not isinstance(obj, Path),
                f"__init__.py defines callable '{attr}' which may cause side effects",
            )


if __name__ == "__main__":
    unittest.main()
