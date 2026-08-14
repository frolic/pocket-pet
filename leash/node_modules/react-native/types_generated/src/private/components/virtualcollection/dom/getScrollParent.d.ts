/**
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 *
 * @generated SignedSource<<da143cc6f517eda55481fb0bcc6158ba>>
 *
 * This file was translated from Flow by scripts/js-api/build-types/index.js.
 * Original file: packages/react-native/src/private/components/virtualcollection/dom/getScrollParent.js
 */

import ReactNativeElement from "../../../webapis/dom/nodes/ReactNativeElement";
/**
 * Finds the nearest ancestor of the supplied node that is a scrollable node.
 *
 * Unlike the web-equivalent function, the return type is nullable because the
 * root is not an implicitly scrollable node.
 */
declare function getScrollParent(node: ReactNativeElement): ReactNativeElement | null;
export default getScrollParent;
