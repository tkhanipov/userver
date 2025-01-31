#include <userver/ugrpc/server/middlewares/field_mask/component.hpp>

#include <userver/components/component_config.hpp>
#include <userver/yaml_config/merge_schemas.hpp>

#include <ugrpc/server/middlewares/field_mask/middleware.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server::middlewares::field_mask {

Component::Component(const components::ComponentConfig& config, const components::ComponentContext& context)
    : MiddlewareFactoryComponentBase(config, context) {}

std::shared_ptr<MiddlewareBase>
Component::CreateMiddleware(const ServiceInfo&, const yaml_config::YamlConfig& middleware_config) const {
    return std::make_shared<Middleware>(
        middleware_config["metadata-field-name"].As<std::string>(kDefaultMetadataFieldName)
    );
}

yaml_config::Schema Component::GetMiddlewareConfigSchema() const { return GetStaticConfigSchema(); }

yaml_config::Schema Component::GetStaticConfigSchema() {
    return yaml_config::MergeSchemas<MiddlewareFactoryComponentBase>(R"(
type: object
description: gRPC service field-mask middleware component
additionalProperties: false
properties:
    metadata-field-name:
        type: string
        description: name of the metadata field to get the field mask from
)");
}

}  // namespace ugrpc::server::middlewares::field_mask

USERVER_NAMESPACE_END
