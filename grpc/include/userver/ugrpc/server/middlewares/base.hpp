#pragma once

/// @file userver/ugrpc/server/middlewares/base.hpp
/// @brief @copybrief ugrpc::server::MiddlewareBase

#include <memory>
#include <optional>

#include <google/protobuf/message.h>

#include <userver/components/component_base.hpp>
#include <userver/utils/function_ref.hpp>
#include <userver/utils/impl/internal_tag.hpp>
#include <userver/yaml_config/schema.hpp>
#include <userver/yaml_config/yaml_config.hpp>

#include <userver/ugrpc/server/call.hpp>
#include <userver/ugrpc/server/middlewares/fwd.hpp>
#include <userver/ugrpc/server/middlewares/groups.hpp>

USERVER_NAMESPACE_BEGIN

namespace ugrpc::server {

struct ServiceInfo;

/// @brief Context for middleware-specific data during gRPC call
class MiddlewareCallContext final {
public:
    /// @cond
    MiddlewareCallContext(
        const Middlewares& middlewares,
        CallAnyBase& call,
        utils::function_ref<void()> user_call,
        const dynamic_config::Snapshot& config,
        ::google::protobuf::Message* request
    );
    /// @endcond

    /// @brief Call next plugin, or gRPC handler if none
    void Next();

    /// @brief Get original gRPC Call
    CallAnyBase& GetCall() const;

    /// @brief Get values extracted from dynamic_config. Snapshot will be
    /// deleted when the last middleware completes
    const dynamic_config::Snapshot& GetInitialDynamicConfig() const;

private:
    void ClearMiddlewaresResources();

    Middlewares::const_iterator middleware_;
    Middlewares::const_iterator middleware_end_;
    utils::function_ref<void()> user_call_;

    CallAnyBase& call_;

    std::optional<dynamic_config::Snapshot> config_;
    ::google::protobuf::Message* request_;
    bool is_called_from_handle_{false};
};

/// @ingroup userver_base_classes
///
/// @brief Base class for server gRPC middleware
class MiddlewareBase {
public:
    MiddlewareBase();
    MiddlewareBase(const MiddlewareBase&) = delete;
    MiddlewareBase& operator=(const MiddlewareBase&) = delete;
    MiddlewareBase& operator=(MiddlewareBase&&) = delete;

    virtual ~MiddlewareBase();

    /// @brief Handles the gRPC request
    /// @note You should call context.Next() inside, otherwise the call will be
    /// dropped
    virtual void Handle(MiddlewareCallContext& context) const = 0;

    /// @brief Request hook. The function is invoked on each request
    virtual void CallRequestHook(const MiddlewareCallContext& context, google::protobuf::Message& request);

    /// @brief Response hook. The function is invoked on each response
    virtual void CallResponseHook(const MiddlewareCallContext& context, google::protobuf::Message& response);
};

/// @ingroup userver_base_classes
///
/// @brief Base class for middleware component
class MiddlewareFactoryComponentBase : public components::ComponentBase {
public:
    MiddlewareFactoryComponentBase(
        const components::ComponentConfig&,
        const components::ComponentContext&,
        ugrpc::middlewares::MiddlewareDependencyBuilder&& builder =
            ugrpc::middlewares::MiddlewareDependencyBuilder().InGroup<groups::User>()
    );

    /// @brief Returns a middleware according to the component's settings
    ///
    /// @param info is a info of a grpc-service
    /// @param middleware_config config for the middleware
    virtual std::shared_ptr<MiddlewareBase>
    CreateMiddleware(const ServiceInfo& info, const yaml_config::YamlConfig& middleware_config) const = 0;

    /// @brief This method should return the schema of a middleware configuration.
    /// Always write `return GetStaticConfigSchema();` in this method
    virtual yaml_config::Schema GetMiddlewareConfigSchema() const { return GetStaticConfigSchema(); }

    static yaml_config::Schema GetStaticConfigSchema();

    /// @cond
    /// Only for internal use.
    const ugrpc::middlewares::impl::MiddlewareDependency& GetMiddlewareDependency(utils::impl::InternalTag) const;

    const formats::yaml::Value& GetGlobalConfig(utils::impl::InternalTag) const;
    /// @endcond

private:
    const ugrpc::middlewares::impl::MiddlewareDependency dependency_;
    const formats::yaml::Value global_config_;
};

/// @ingroup userver_middlewares
///
/// @brief A short-cut for defining a middleware-factory
template <typename Middleware>
class SimpleMiddlewareFactoryComponent final : public MiddlewareFactoryComponentBase {
public:
    static constexpr std::string_view kName = Middleware::kName;

    SimpleMiddlewareFactoryComponent(
        const components::ComponentConfig& config,
        const components::ComponentContext& context
    )
        : MiddlewareFactoryComponentBase(
              config,
              context,
              ugrpc::middlewares::MiddlewareDependencyBuilder{Middleware::kDependency}
          ) {}

private:
    std::shared_ptr<MiddlewareBase> CreateMiddleware(const ServiceInfo&, const yaml_config::YamlConfig&)
        const override {
        return std::make_shared<Middleware>();
    }
};

}  // namespace ugrpc::server

template <typename Middleware>
inline constexpr bool components::kHasValidate<ugrpc::server::SimpleMiddlewareFactoryComponent<Middleware>> = true;

USERVER_NAMESPACE_END
