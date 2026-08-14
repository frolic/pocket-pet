/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @generated SignedSource<<502bc5e7d28680ebf63c67cec2aebbe9>>
 *
 * This file was translated from Flow by scripts/js-api/build-types/index.js.
 * Original file: packages/react-native/src/private/components/virtualcollection/column/VirtualColumn.js
 */

import type { Item, VirtualCollection } from "../Virtual";
import * as React from "react";
declare const $$VirtualColumn: <TItem extends Item>(props: {
  children: (item: TItem, key: string) => React.ReactNode;
  items: VirtualCollection<TItem>;
  itemToKey?: ($$PARAM_0$$: TItem) => string;
  removeClippedSubviews?: boolean;
  testID?: null | undefined | string;
}) => React.ReactNode;
declare type $$VirtualColumn = typeof $$VirtualColumn;
export default $$VirtualColumn;
