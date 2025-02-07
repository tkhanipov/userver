import pathlib
import tempfile

from grpc_reflection.v1alpha.proto_reflection_descriptor_database import ServerReflectionStub
import pytest

pytest_plugins = ['pytest_userver.plugins.grpc']

USERVER_CONFIG_HOOKS = ['prepare_service_config']


@pytest.fixture(scope='session')
def unix_socket_path(tmp_path_factory) -> pathlib.Path:
    with tempfile.TemporaryDirectory(prefix='userver-grpc-socket-') as name:
        yield pathlib.Path(name) / 's'


@pytest.fixture(scope='session')
def grpc_service_endpoint(service_config) -> str:
    components = service_config['components_manager']['components']
    return f'unix:{components["grpc-server"]["unix-socket-path"]}'


@pytest.fixture(name='prepare_service_config', scope='session')
def _prepare_service_config(unix_socket_path):
    def patch_config(config, config_vars):
        components = config['components_manager']['components']
        components['grpc-server']['unix-socket-path'] = str(unix_socket_path)

    return patch_config


@pytest.fixture
def grpc_reflection_client(grpc_channel, service_client):
    return ServerReflectionStub(grpc_channel)
