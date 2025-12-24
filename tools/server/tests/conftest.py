import os
import pytest
from utils import *

# Configure pytest-asyncio to use auto mode
pytest_plugins = ('pytest_asyncio',)


# ref: https://stackoverflow.com/questions/22627659/run-code-before-and-after-each-test-in-py-test
@pytest.fixture(autouse=True)
def stop_server_after_each_test():
    # do nothing before each test
    yield
    # stop all servers after each test
    instances = set(
        server_instances
    )  # copy the set to prevent 'Set changed size during iteration'
    for server in instances:
        server.stop()


@pytest.fixture(scope="module", autouse=True)
def do_something():
    # this will be run once per test session, before any tests
    # Skip if SKIP_MODEL_PRELOAD is set (useful for tests that use local models)
    if os.environ.get("SKIP_MODEL_PRELOAD"):
        return
    ServerPreset.load_all()
