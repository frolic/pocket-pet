/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @generated SignedSource<<a6335eb8723e537d5864fde5a5e4d6c1>>
 *
 * This file was translated from Flow by scripts/js-api/build-types/index.js.
 * Original file: packages/react-native/src/private/components/virtualcollection/row/VirtualRow.js
 */

import type { Item, VirtualCollection } from "../Virtual";
import * as React from "react";
declare const $$VirtualRow: <TItem extends Item>(props: {
  children: (item: TItem, key: string) => React.ReactNode;
  items: VirtualCollection<TItem>;
  itemToKey?: ($$PARAM_0$$: TItem) => string;
  removeClippedSubviews?: boolean;
  testID?: null | undefined | string;
}) => React.ReactNode;
declare type $$VirtualRow = typeof $$VirtualRow;
export default $$VirtualRow;
