// Copyright (c) 2022 The Brave Authors. All rights reserved.
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this file,
// you can obtain one at https://mozilla.org/MPL/2.0/.

import {
  createDraftSafeSelector,
  createEntityAdapter,
  EntityAdapter,
  EntityId,
  EntityState,
} from '@reduxjs/toolkit'
import { BraveWallet } from '../../../constants/types'
import { getEntitiesListFromEntityState } from '../../../utils/entities.utils'

export const getNetworkId = ({ chainId }: { chainId: string }): string =>
  chainId

export type NetworkEntityAdaptor = EntityAdapter<BraveWallet.NetworkInfo> & {
  selectId: (network: BraveWallet.NetworkInfo) => EntityId
}

export const networkEntityAdapter: NetworkEntityAdaptor =
  createEntityAdapter<BraveWallet.NetworkInfo>({
    selectId: getNetworkId,
  })

export type NetworksRegistry = EntityState<BraveWallet.NetworkInfo> & {
  hiddenIds: EntityId[]
  visibleIds: EntityId[]
  offRampIds: EntityId[]
  ankrChainIds: EntityId[]
  swapChainIds: EntityId[]
}

export const emptyNetworksRegistry: NetworksRegistry = {
  ...networkEntityAdapter.getInitialState(),
  hiddenIds: [],
  visibleIds: [],
  offRampIds: [],
  ankrChainIds: [],
  swapChainIds: [],
}

const selectNetworksRegistryFromQueryResult = (
  networksRegistry: NetworksRegistry | undefined,
) => {
  return networksRegistry ?? emptyNetworksRegistry
}

export const networkSelectors = {
  ...networkEntityAdapter.getSelectors(selectNetworksRegistryFromQueryResult),
  selectOffRampNetworks: createDraftSafeSelector(
    [selectNetworksRegistryFromQueryResult],
    (registry) => getEntitiesListFromEntityState(registry, registry.offRampIds),
  ),
  selectVisibleNetworks: createDraftSafeSelector(
    [selectNetworksRegistryFromQueryResult],
    (registry) => getEntitiesListFromEntityState(registry, registry.visibleIds),
  ),
  selectSwapNetworks: createDraftSafeSelector(
    [selectNetworksRegistryFromQueryResult],
    (registry) =>
      getEntitiesListFromEntityState(registry, registry.swapChainIds),
  ),
}
