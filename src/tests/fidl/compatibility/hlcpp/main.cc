// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/test/compatibility/cpp/fidl.h>
#include <fidl/test/imported/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/fidl/cpp/binding_set.h>
#include <lib/fidl/cpp/interface_request.h>
#include <lib/sys/cpp/component_context.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/channel.h>
#include <zircon/types.h>

#include <cstdlib>
#include <string>

#include "src/tests/fidl/compatibility/hlcpp_client_app.h"

namespace fidl {
namespace test {
namespace compatibility {

class EchoServerApp : public Echo {
 public:
  explicit EchoServerApp(async::Loop* loop)
      : loop_(loop), context_(sys::ComponentContext::CreateAndServeOutgoingDirectory()) {
    context_->outgoing()->AddPublicService(bindings_.GetHandler(this));
  }

  ~EchoServerApp() {}

  void EchoMinimal(std::string forward_to_server, EchoMinimalCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoMinimal("", [this, &called_back, &callback]() {
        called_back = true;
        callback();
        loop_->Quit();
      });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      callback();
    }
  }

  void EchoMinimalWithError(std::string forward_to_server, RespondWith result_variant,
                            EchoMinimalWithErrorCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoMinimalWithError(
          "", result_variant,
          [this, &called_back, &callback](Echo_EchoMinimalWithError_Result result) {
            called_back = true;
            callback(std::move(result));
            loop_->Quit();
          });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      Echo_EchoMinimalWithError_Result result;
      if (result_variant == RespondWith::ERR) {
        result.set_err(0u);
      } else {
        result.set_response(Echo_EchoMinimalWithError_Response());
      }
      callback(std::move(result));
    }
  }

  void EchoMinimalNoRetVal(std::string forward_to_server) override {
    if (!forward_to_server.empty()) {
      std::unique_ptr<EchoClientApp> app(new EchoClientApp);
      app->echo().set_error_handler([this, forward_to_server](zx_status_t status) {
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app->Connect();
      app->echo().events().EchoMinimalEvent = [this]() { this->HandleEchoMinimalEvent(); };
      app->echo()->EchoMinimalNoRetVal("");
      client_apps_.push_back(std::move(app));
    } else {
      for (const auto& binding : bindings_.bindings()) {
        binding->events().EchoMinimalEvent();
      }
    }
  }

  void EchoStruct(Struct value, std::string forward_to_server,
                  EchoStructCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoStruct(std::move(value), "", [this, &called_back, &callback](Struct resp) {
        called_back = true;
        callback(std::move(resp));
        loop_->Quit();
      });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      callback(std::move(value));
    }
  }

  void EchoStructWithError(Struct value, default_enum err, std::string forward_to_server,
                           RespondWith result_variant,
                           EchoStructWithErrorCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoStructWithError(
          std::move(value), std::move(err), "", result_variant,
          [this, &called_back, &callback](Echo_EchoStructWithError_Result result) {
            called_back = true;
            callback(std::move(result));
            loop_->Quit();
          });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      Echo_EchoStructWithError_Result result;
      if (result_variant == RespondWith::ERR) {
        result.set_err(err);
      } else {
        result.set_response(Echo_EchoStructWithError_Response(std::move(value)));
      }
      callback(std::move(result));
    }
  }

  void EchoStructNoRetVal(Struct value, std::string forward_to_server) override {
    if (!forward_to_server.empty()) {
      std::unique_ptr<EchoClientApp> app(new EchoClientApp);
      app->echo().set_error_handler([this, forward_to_server](zx_status_t status) {
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app->Connect();
      app->echo().events().EchoEvent = [this](Struct resp) {
        this->HandleEchoEvent(std::move(resp));
      };
      app->echo()->EchoStructNoRetVal(std::move(value), "");
      client_apps_.push_back(std::move(app));
    } else {
      for (const auto& binding : bindings_.bindings()) {
        Struct to_send;
        value.Clone(&to_send);
        binding->events().EchoEvent(std::move(to_send));
      }
    }
  }

  void EchoArrays(ArraysStruct value, std::string forward_to_server,
                  EchoArraysCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoArrays(std::move(value), "",
                             [this, &called_back, &callback](ArraysStruct resp) {
                               called_back = true;
                               callback(std::move(resp));
                               loop_->Quit();
                             });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      callback(std::move(value));
    }
  }

  void EchoArraysWithError(ArraysStruct value, default_enum err, std::string forward_to_server,
                           RespondWith result_variant,
                           EchoArraysWithErrorCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoArraysWithError(
          std::move(value), std::move(err), "", result_variant,
          [this, &called_back, &callback](Echo_EchoArraysWithError_Result result) {
            called_back = true;
            callback(std::move(result));
            loop_->Quit();
          });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      Echo_EchoArraysWithError_Result result;
      if (result_variant == RespondWith::ERR) {
        result.set_err(err);
      } else {
        result.set_response(Echo_EchoArraysWithError_Response(std::move(value)));
      }
      callback(std::move(result));
    }
  }

  void EchoVectors(VectorsStruct value, std::string forward_to_server,
                   EchoVectorsCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoVectors(std::move(value), "",
                              [this, &called_back, &callback](VectorsStruct resp) {
                                called_back = true;
                                callback(std::move(resp));
                                loop_->Quit();
                              });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      callback(std::move(value));
    }
  }

  void EchoVectorsWithError(VectorsStruct value, default_enum err, std::string forward_to_server,
                            RespondWith result_variant,
                            EchoVectorsWithErrorCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoVectorsWithError(
          std::move(value), std::move(err), "", result_variant,
          [this, &called_back, &callback](Echo_EchoVectorsWithError_Result result) {
            called_back = true;
            callback(std::move(result));
            loop_->Quit();
          });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      Echo_EchoVectorsWithError_Result result;
      if (result_variant == RespondWith::ERR) {
        result.set_err(err);
      } else {
        result.set_response(Echo_EchoVectorsWithError_Response(std::move(value)));
      }
      callback(std::move(result));
    }
  }

  void EchoTable(AllTypesTable value, std::string forward_to_server,
                 EchoTableCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoTable(std::move(value), "",
                            [this, &called_back, &callback](AllTypesTable resp) {
                              called_back = true;
                              callback(std::move(resp));
                              loop_->Quit();
                            });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      callback(std::move(value));
    }
  }

  void EchoTableWithError(AllTypesTable value, default_enum err, std::string forward_to_server,
                          RespondWith result_variant,
                          EchoTableWithErrorCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoTableWithError(
          std::move(value), std::move(err), "", result_variant,
          [this, &called_back, &callback](Echo_EchoTableWithError_Result result) {
            called_back = true;
            callback(std::move(result));
            loop_->Quit();
          });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      Echo_EchoTableWithError_Result result;
      if (result_variant == RespondWith::ERR) {
        result.set_err(err);
      } else {
        result.set_response(Echo_EchoTableWithError_Response(std::move(value)));
      }
      callback(std::move(result));
    }
  }

  void EchoXunions(std::vector<AllTypesXunion> value, std::string forward_to_server,
                   EchoXunionsCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoXunions(std::move(value), "",
                              [this, &called_back, &callback](std::vector<AllTypesXunion> resp) {
                                called_back = true;
                                callback(std::move(resp));
                                loop_->Quit();
                              });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      callback(std::move(value));
    }
  }

  void EchoXunionsWithError(std::vector<AllTypesXunion> value, default_enum err,
                            std::string forward_to_server, RespondWith result_variant,
                            EchoXunionsWithErrorCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoXunionsWithError(
          std::move(value), std::move(err), "", result_variant,
          [this, &called_back, &callback](Echo_EchoXunionsWithError_Result result) {
            called_back = true;
            callback(std::move(result));
            loop_->Quit();
          });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      Echo_EchoXunionsWithError_Result result;
      if (result_variant == RespondWith::ERR) {
        result.set_err(err);
      } else {
        result.set_response(Echo_EchoXunionsWithError_Response(std::move(value)));
      }
      callback(std::move(result));
    }
  }

  void EchoNamedStruct(imported::SimpleStruct value, std::string forward_to_server,
                       EchoNamedStructCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoNamedStruct(std::move(value), "",
                                  [this, &called_back, &callback](imported::SimpleStruct resp) {
                                    called_back = true;
                                    callback(std::move(resp));
                                    loop_->Quit();
                                  });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      callback(std::move(value));
    }
  }

  void EchoNamedStructWithError(imported::SimpleStruct value, uint32_t err,
                                std::string forward_to_server,
                                imported::WantResponse result_variant,
                                EchoNamedStructWithErrorCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoNamedStructWithError(
          std::move(value), std::move(err), "", result_variant,
          [this, &called_back, &callback](Echo_EchoNamedStructWithError_Result result) {
            called_back = true;
            callback(std::move(result));
            loop_->Quit();
          });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      Echo_EchoNamedStructWithError_Result result;
      if (result_variant == imported::WantResponse::ERR) {
        result.set_err(err);
      } else {
        result.set_response(imported::ResponseStruct{std::move(value)});
      }
      callback(std::move(result));
    }
  }

  void EchoNamedStructNoRetVal(imported::SimpleStruct value,
                               std::string forward_to_server) override {
    if (!forward_to_server.empty()) {
      std::unique_ptr<EchoClientApp> app(new EchoClientApp);
      app->echo().set_error_handler([this, forward_to_server](zx_status_t status) {
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app->Connect();
      app->echo().events().OnEchoNamedEvent = [this](imported::SimpleStruct resp) {
        this->HandleOnEchoNamedEvent(std::move(resp));
      };
      app->echo()->EchoNamedStructNoRetVal(std::move(value), "");
      client_apps_.push_back(std::move(app));
    } else {
      for (const auto& binding : bindings_.bindings()) {
        imported::SimpleStruct to_send;
        value.Clone(&to_send);
        binding->events().OnEchoNamedEvent(std::move(to_send));
      }
    }
  }

  void EchoTablePayload(RequestTable payload, EchoTablePayloadCallback callback) override {
    if (payload.has_forward_to_server()) {
      EchoClientApp app;
      const std::string& forward_to_server = payload.forward_to_server();
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      payload.clear_forward_to_server();
      app.echo()->EchoTablePayload(std::move(payload),
                                   [this, &called_back, &callback](ResponseTable resp) {
                                     called_back = true;
                                     callback(std::move(resp));
                                     loop_->Quit();
                                   });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      ResponseTable resp;
      resp.set_value(payload.value());
      callback(std::move(resp));
    }
  }

  void EchoTablePayloadNoRetVal(RequestTable payload) override {
    if (payload.has_forward_to_server()) {
      std::unique_ptr<EchoClientApp> app(new EchoClientApp);
      const std::string& forward_to_server = payload.forward_to_server();
      app->echo().set_error_handler([this, forward_to_server](zx_status_t status) {
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app->Connect();
      app->echo().events().OnEchoTablePayloadEvent = [this](ResponseTable resp) {
        this->HandleOnTablePayloadEvent(std::move(resp));
      };
      payload.clear_forward_to_server();
      app->echo()->EchoTablePayloadNoRetVal(std::move(payload));
      client_apps_.push_back(std::move(app));
    } else {
      for (const auto& binding : bindings_.bindings()) {
        ResponseTable resp;
        resp.set_value(payload.value());
        binding->events().OnEchoTablePayloadEvent(std::move(resp));
      }
    }
  }

  void EchoTablePayloadWithError(EchoEchoTablePayloadWithErrorRequest payload,
                                 EchoTablePayloadWithErrorCallback callback) override {
    if (payload.has_forward_to_server()) {
      EchoClientApp app;
      const std::string& forward_to_server = payload.forward_to_server();
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      payload.clear_forward_to_server();
      app.echo()->EchoTablePayloadWithError(
          std::move(payload),
          [this, &called_back, &callback](Echo_EchoTablePayloadWithError_Result res) {
            called_back = true;
            callback(std::move(res));
            loop_->Quit();
          });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      Echo_EchoTablePayloadWithError_Result result;
      if (payload.result_variant() == RespondWith::ERR) {
        result.set_err(payload.result_err());
      } else {
        ResponseTable resp;
        resp.set_value(payload.value());
        result.set_response(std::move(resp));
      }
      callback(std::move(result));
    }
  }

  void EchoTableRequestComposed(
      ::fidl::test::imported::ComposedEchoTableRequestComposedRequest payload,
      EchoTableRequestComposedCallback callback) override {
    if (payload.has_forward_to_server()) {
      EchoClientApp app;
      const std::string& forward_to_server = payload.forward_to_server();
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      payload.clear_forward_to_server();
      app.echo()->EchoTableRequestComposed(
          std::move(payload), [this, &called_back, &callback](imported::SimpleStruct resp) {
            called_back = true;
            callback(std::move(resp));
            loop_->Quit();
          });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      imported::SimpleStruct resp = {.f1 = true, .f2 = payload.value()};
      callback(std::move(resp));
    }
  }

  void EchoUnionPayload(RequestUnion payload, EchoUnionPayloadCallback callback) override {
    const std::string& forward_to_server = payload.is_signed_()
                                               ? payload.signed_().forward_to_server
                                               : payload.unsigned_().forward_to_server;
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      if (payload.is_signed_()) {
        payload.signed_().forward_to_server = "";
      } else {
        payload.unsigned_().forward_to_server = "";
      }
      app.echo()->EchoUnionPayload(std::move(payload),
                                   [this, &called_back, &callback](ResponseUnion resp) {
                                     called_back = true;
                                     callback(std::move(resp));
                                     loop_->Quit();
                                   });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      ResponseUnion resp;
      if (payload.is_signed_()) {
        resp.set_signed_(payload.signed_().value);
      } else {
        resp.set_unsigned_(payload.unsigned_().value);
      }
      callback(std::move(resp));
    }
  }

  void EchoUnionPayloadWithError(EchoEchoUnionPayloadWithErrorRequest payload,
                                 EchoUnionPayloadWithErrorCallback callback) override {
    const std::string& forward_to_server = payload.is_signed_()
                                               ? payload.signed_().forward_to_server
                                               : payload.unsigned_().forward_to_server;
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      if (payload.is_signed_()) {
        payload.signed_().forward_to_server = "";
      } else {
        payload.unsigned_().forward_to_server = "";
      }
      app.echo()->EchoUnionPayloadWithError(
          std::move(payload),
          [this, &called_back, &callback](Echo_EchoUnionPayloadWithError_Result res) {
            called_back = true;
            callback(std::move(res));
            loop_->Quit();
          });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
    } else {
      const RespondWith result_variant = payload.is_signed_() ? payload.signed_().result_variant
                                                              : payload.unsigned_().result_variant;
      Echo_EchoUnionPayloadWithError_Result result;
      ResponseUnion resp;
      if (result_variant == RespondWith::ERR) {
        result.set_err(payload.is_signed_() ? payload.signed_().result_err
                                            : payload.unsigned_().result_err);
      } else {
        if (payload.is_signed_()) {
          resp.set_signed_(payload.signed_().value);
        } else {
          resp.set_unsigned_(payload.unsigned_().value);
        }
        result.set_response(std::move(resp));
      }
      callback(std::move(result));
    }
  }

  void EchoUnionPayloadNoRetVal(RequestUnion payload) override {
    const std::string& forward_to_server = payload.is_signed_()
                                               ? payload.signed_().forward_to_server
                                               : payload.unsigned_().forward_to_server;
    if (!forward_to_server.empty()) {
      std::unique_ptr<EchoClientApp> app(new EchoClientApp);
      app->echo().set_error_handler([this, forward_to_server](zx_status_t status) {
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app->Connect();
      app->echo().events().OnEchoUnionPayloadEvent = [this](ResponseUnion resp) {
        this->HandleOnUnionPayloadEvent(std::move(resp));
      };
      if (payload.is_signed_()) {
        payload.signed_().forward_to_server = "";
      } else {
        payload.unsigned_().forward_to_server = "";
      }
      app->echo()->EchoUnionPayloadNoRetVal(std::move(payload));
      client_apps_.push_back(std::move(app));
    } else {
      for (const auto& binding : bindings_.bindings()) {
        ResponseUnion resp;
        if (payload.is_signed_()) {
          resp.set_signed_(payload.signed_().value);
        } else {
          resp.set_unsigned_(payload.unsigned_().value);
        }
        binding->events().OnEchoUnionPayloadEvent(std::move(resp));
      }
    }
  }

  void EchoUnionResponseWithErrorComposed(
      int64_t value, bool want_absolute_value, std::string forward_to_server, uint32_t err,
      imported::WantResponse result_variant,
      EchoUnionResponseWithErrorComposedCallback callback) override {
    if (!forward_to_server.empty()) {
      EchoClientApp app;
      bool failed = false;
      app.echo().set_error_handler([this, &forward_to_server, &failed](zx_status_t status) {
        failed = true;
        loop_->Quit();
        FX_LOGS(ERROR) << "error communicating with " << forward_to_server << ": " << status;
      });
      app.Connect();
      bool called_back = false;
      app.echo()->EchoUnionResponseWithErrorComposed(
          value, want_absolute_value, "", err, result_variant,
          [this, &called_back,
           &callback](imported::Composed_EchoUnionResponseWithErrorComposed_Result resp) {
            called_back = true;
            callback(std::move(resp));
            loop_->Quit();
          });
      while (!called_back && !failed) {
        loop_->Run();
      }
      loop_->ResetQuit();
      return;
    }

    imported::Composed_EchoUnionResponseWithErrorComposed_Result result;
    if (result_variant == imported::WantResponse::ERR) {
      result.set_err(err);
    } else if (want_absolute_value) {
      result.set_response(
          ::fidl::test::imported::Composed_EchoUnionResponseWithErrorComposed_Response::
              WithUnsigned_(static_cast<uint64_t>(std::abs(value))));
    } else {
      result.set_response(
          ::fidl::test::imported::Composed_EchoUnionResponseWithErrorComposed_Response::WithSigned_(
              std::move(value)));
    }
    callback(std::move(result));
  }

 private:
  void HandleEchoEvent(Struct value) {
    for (const auto& binding : bindings_.bindings()) {
      Struct to_send;
      value.Clone(&to_send);
      binding->events().EchoEvent(std::move(to_send));
    }
  }
  void HandleEchoMinimalEvent() {
    for (const auto& binding : bindings_.bindings()) {
      binding->events().EchoMinimalEvent();
    }
  }
  void HandleOnEchoNamedEvent(imported::SimpleStruct value) {
    for (const auto& binding : bindings_.bindings()) {
      imported::SimpleStruct to_send;
      value.Clone(&to_send);
      binding->events().OnEchoNamedEvent(std::move(to_send));
    }
  }
  void HandleOnTablePayloadEvent(ResponseTable payload) {
    for (const auto& binding : bindings_.bindings()) {
      ResponseTable to_send;
      payload.Clone(&to_send);
      binding->events().OnEchoTablePayloadEvent(std::move(to_send));
    }
  }
  void HandleOnUnionPayloadEvent(ResponseUnion payload) {
    for (const auto& binding : bindings_.bindings()) {
      ResponseUnion to_send;
      payload.Clone(&to_send);
      binding->events().OnEchoUnionPayloadEvent(std::move(to_send));
    }
  }

  EchoPtr server_ptr;
  EchoServerApp(const EchoServerApp&) = delete;
  EchoServerApp& operator=(const EchoServerApp&) = delete;

  async::Loop* loop_;
  std::unique_ptr<sys::ComponentContext> context_;
  fidl::BindingSet<Echo> bindings_;
  std::vector<std::unique_ptr<EchoClientApp>> client_apps_;
};

}  // namespace compatibility
}  // namespace test
}  // namespace fidl

int main(int argc, const char** argv) {
  // The FIDL support lib requires async_get_default_dispatcher() to return
  // non-null.
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  fidl::test::compatibility::EchoServerApp app(&loop);
  loop.Run();
  return EXIT_SUCCESS;
}
