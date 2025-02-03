#include <userver/ugrpc/server/service_component_base.hpp>

#include <userver/components/component_config.hpp>
#include <userver/components/component_context.hpp>
#include <userver/formats/common/merge.hpp>
#include <userver/formats/yaml/value_builder.hpp>
#include <userver/utils/assert.hpp>
#include <userver/yaml_config/merge_schemas.hpp>
#include <userver/yaml_config/yaml_config.hpp>

#include <ugrpc/server/impl/parse_config.hpp>
#include <userver/ugrpc/middlewares/pipeline.hpp>
#include <userver/ugrpc/server/middlewares/base.hpp>
#include <userver/ugrpc/server/server_component.hpp>
#include <userver/ugrpc/server/service_base.hpp>
#include <userver/yaml_config/impl/validate_static_config.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server {

ServiceComponentBase::ServiceComponentBase(
    const components::ComponentConfig& config,
    const components::ComponentContext& context
)
    : ComponentBase(config, context),
      server_(context.FindComponent<ServerComponent>()),
      config_(server_.ParseServiceConfig(config, context)),
      info_{config.Name()} {
    const auto conf = config.As<middlewares::impl::MiddlewareServiceConfig>();
    const auto& middlewares = config["middlewares"];
    const auto& pipeline = context.FindComponent<middlewares::MiddlewarePipelineComponent>().GetPipeline();

    for (const auto& mid : pipeline.GetPerServiceMiddlewares(conf)) {
        const auto& base = context.FindComponent<MiddlewareFactoryComponentBase>(mid);
        CreateAndPushMiddleware(base, middlewares[mid]);
    }
}

void ServiceComponentBase::CreateAndPushMiddleware(
    const MiddlewareFactoryComponentBase& base,
    const yaml_config::YamlConfig& middleware
) {
    formats::yaml::ValueBuilder builder{base.GetGlobalConfig(utils::impl::InternalTag{})};

    if (!middleware.IsMissing()) {
        formats::common::Merge(builder, middleware.As<formats::yaml::Value>());
        auto schema = base.GetMiddlewareConfigSchema();
        schema.properties->erase("load-enabled");
        yaml_config::impl::Validate(middleware, schema);
    }
    config_.middlewares.push_back(
        base.CreateMiddleware(info_, yaml_config::YamlConfig{builder.ExtractValue(), formats::yaml::Value{}})
    );
}

void ServiceComponentBase::RegisterService(ServiceBase& service) {
    UINVARIANT(!registered_.exchange(true), "Register must only be called once");

    server_.GetServer().AddService(service, std::move(config_));
}

void ServiceComponentBase::RegisterService(GenericServiceBase& service) {
    UINVARIANT(!registered_.exchange(true), "Register must only be called once");
    server_.GetServer().AddService(service, std::move(config_));
}

yaml_config::Schema ServiceComponentBase::GetStaticConfigSchema() {
    return yaml_config::MergeSchemas<components::ComponentBase>(R"(
type: object
description: base class for all the gRPC service components
additionalProperties: false
properties:
    task-processor:
        type: string
        description: the task processor to use for responses
        defaultDescription: uses grpc-server.service-defaults.task-processor
    disable-user-pipeline-middlewares:
        type: boolean
        description: flag to disable groups::User middlewares from pipeline
        defaultDescription: false
    disable-all-pipeline-middlewares:
        type: boolean
        description: flag to disable all middlewares from pipline
        defaultDescription: false
    middlewares:
        type: object
        description: overloads of configs of middlewares per service
        additionalProperties:
            type: object
            description: a middleware config
            additionalProperties: true
            properties:
                enabled:
                    type: boolean
                    description: enable middleware in the list
        properties: {}
)");
}

}  // namespace ugrpc::server

USERVER_NAMESPACE_END
