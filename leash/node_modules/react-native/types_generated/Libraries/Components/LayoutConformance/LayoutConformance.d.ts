/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @generated SignedSource<<d967f474c0e6cfa3f4ee2abdfaab4e22>>
 *
 * This file was translated from Flow by scripts/js-api/build-types/index.js.
 * Original file: packages/react-native/Libraries/Components/LayoutConformance/LayoutConformance.js
 */

import * as React from "react";
export type LayoutConformanceProps = Readonly<{
  /**
   * strict: Layout in accordance with W3C spec, even when breaking
   * compatibility: Layout with the same behavior as previous versions of React Native
   */
  mode: "strict" | "compatibility";
  children: React.ReactNode;
}>;
declare function LayoutConformance(props: LayoutConformanceProps): React.ReactNode;
export default LayoutConformance;
