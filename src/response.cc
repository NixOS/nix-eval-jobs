#include <nlohmann/json.hpp>
#include <nix/util/json-utils.hh>
#include <nix/util/util.hh> // for overloaded
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "response.hh"
#include "drv.hh"

auto joinAttrPath(const nlohmann::json &attrPath) -> std::string {
    std::string joined;
    for (const auto &element : attrPath) {
        auto part = element.get<std::string>();
        if (part.find('.') != std::string::npos) {
            part = "\"" + part + "\"";
        }
        joined += joined.empty() ? part : "." + part;
    }
    return joined;
}

namespace nlohmann {

using nix::get;
using nix::getBoolean;
using nix::getObject;
using nix::getString;
using nix::overloaded;
using nix::valueAt;

void adl_serializer<Response>::to_json(json &res, const Response &response) {
    res = json{
        {"attr", response.attr},
        {"attrPath", response.attrPath},
    };

    std::visit(overloaded{
                   [&](const Response::Job &job) -> void {
                       // Merge Drv fields at the top level (flat structure)
                       res.update(json(job.drv));
                       if (job.extraValue) {
                           res["extraValue"] = *job.extraValue;
                       }
                   },
                   [&](const Response::Attrs &attrs) -> void {
                       res["attrs"] = attrs.attrs;
                   },
                   [&](const Response::Error &error) -> void {
                       res["error"] = error.error;
                       res["fatal"] = error.fatal;
                   },
               },
               response.payload);
}

auto adl_serializer<Response>::from_json(const json &_json) -> Response {
    const auto &json = getObject(_json);

    auto attr = getString(valueAt(json, "attr"));
    std::vector<std::string> attrPath = valueAt(json, "attrPath");

    auto makeResponse = [&](Response::Payload payload) -> Response {
        return Response{
            .attr = std::move(attr),
            .attrPath = std::move(attrPath),
            .payload = std::move(payload),
        };
    };

    if (const auto *attrs = get(json, "attrs")) {
        return makeResponse(Response::Attrs{*attrs});
    }
    if (const auto *error = get(json, "error")) {
        Response::Error err{.error = getString(*error)};
        if (const auto *fatalJson = get(json, "fatal")) {
            err.fatal = getBoolean(*fatalJson);
        }
        return makeResponse(std::move(err));
    }
    if (get(json, "drvPath") != nullptr) {
        auto drv = adl_serializer<Drv>::from_json(_json);
        std::optional<nlohmann::json> extraValue;
        if (const auto *extra = get(json, "extraValue")) {
            extraValue = *extra;
        }
        return makeResponse(Response::Job{
            std::move(drv),
            std::move(extraValue),
        });
    }

    throw std::invalid_argument(
        "Response JSON must contain 'attrs', 'error', or 'drvPath'");
}

} // namespace nlohmann
