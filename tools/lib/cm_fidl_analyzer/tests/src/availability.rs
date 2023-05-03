// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod tests {
    use {
        crate::routing::RoutingTestBuilderForAnalyzer,
        routing_test_helpers::availability::CommonAvailabilityTest,
    };

    #[fuchsia::test]
    async fn offer_availability_successful_routes() {
        CommonAvailabilityTest::<RoutingTestBuilderForAnalyzer>::new()
            .test_offer_availability_successful_routes()
            .await
    }

    #[fuchsia::test]
    async fn offer_availability_invalid_routes() {
        CommonAvailabilityTest::<RoutingTestBuilderForAnalyzer>::new()
            .test_offer_availability_invalid_routes()
            .await
    }

    #[fuchsia::test]
    async fn expose_availability_successful_routes() {
        CommonAvailabilityTest::<RoutingTestBuilderForAnalyzer>::new()
            .test_expose_availability_successful_routes()
            .await
    }

    #[fuchsia::test]
    async fn expose_availability_invalid_routes() {
        CommonAvailabilityTest::<RoutingTestBuilderForAnalyzer>::new()
            .test_expose_availability_invalid_routes()
            .await
    }
}
