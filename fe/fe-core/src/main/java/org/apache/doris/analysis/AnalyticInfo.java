// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.
// This file is copied from
// https://github.com/apache/impala/blob/branch-2.9.0/fe/src/main/java/org/apache/impala/AnalyticInfo.java
// and modified by Doris

package org.apache.doris.analysis;

import org.apache.doris.catalog.Type;

import com.google.common.base.MoreObjects;
import com.google.common.base.Preconditions;
import com.google.common.collect.Lists;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;

import java.util.ArrayList;
import java.util.List;

/**
 * Encapsulates the analytic functions found in a single select block plus
 * the corresponding analytic result tuple and its substitution map.
 */
public final class AnalyticInfo extends AggregateInfoBase {
    private static final Logger LOG = LoggerFactory.getLogger(AnalyticInfo.class);

    // All unique analytic exprs of a select block. Used to populate
    // super.aggregateExprs_ based on AnalyticExpr.getFnCall() for each analytic expr
    // in this list.
    private final ArrayList<Expr> analyticExprs;

    // Intersection of the partition exps of all the analytic functions.
    private final List<Expr> commonPartitionExprs;

    // map from analyticExprs_ to their corresponding analytic tuple slotrefs
    private final ExprSubstitutionMap analyticTupleSmap;

    private AnalyticInfo(ArrayList<Expr> analyticExprs) {
        super(new ArrayList<Expr>(), new ArrayList<FunctionCallExpr>());
        this.analyticExprs = Expr.cloneList(analyticExprs);
        // Extract the analytic function calls for each analytic expr.
        for (Expr analyticExpr : analyticExprs) {
            aggregateExprs.add(((AnalyticExpr) analyticExpr).getFnCall());
        }
        analyticTupleSmap = new ExprSubstitutionMap();
        commonPartitionExprs = computeCommonPartitionExprs();
    }

    /**
     * C'tor for cloning.
     */
    private AnalyticInfo(AnalyticInfo other) {
        super(other);
        analyticExprs =
                (other.analyticExprs != null) ? Expr.cloneList(other.analyticExprs) : null;
        analyticTupleSmap = other.analyticTupleSmap.clone();
        commonPartitionExprs = Expr.cloneList(other.commonPartitionExprs);
    }

    public ArrayList<Expr> getAnalyticExprs() {
        return analyticExprs;
    }

    public ExprSubstitutionMap getSmap() {
        return analyticTupleSmap;
    }

    public List<Expr> getCommonPartitionExprs() {
        return commonPartitionExprs;
    }

    /**
     * Returns the intersection of the partition exprs of all the
     * analytic functions.
     */
    private List<Expr> computeCommonPartitionExprs() {
        List<Expr> result = Lists.newArrayList();
        for (Expr analyticExpr : analyticExprs) {
            Preconditions.checkState(analyticExpr.isAnalyzed());
            List<Expr> partitionExprs = ((AnalyticExpr) analyticExpr).getPartitionExprs();
            if (partitionExprs == null) {
                continue;
            }
            if (result.isEmpty()) {
                result.addAll(partitionExprs);
            } else {
                result.retainAll(partitionExprs);
                if (result.isEmpty()) {
                    break;
                }
            }
        }
        return result;
    }

    /**
     * Validates internal state: Checks that the number of materialized slots of the
     * analytic tuple corresponds to the number of materialized analytic functions. Also
     * checks that the return types of the analytic exprs correspond to the slots in the
     * analytic tuple.
     */
    public void checkConsistency() {
        ArrayList<SlotDescriptor> slots = intermediateTupleDesc.getSlots();

        // Check materialized slots.
        int numMaterializedSlots = 0;
        for (SlotDescriptor slotDesc : slots) {
            if (slotDesc.isMaterialized()) {
                ++numMaterializedSlots;
            }
        }
        Preconditions.checkState(numMaterializedSlots == materializedSlots.size());

        // Check that analytic expr return types match the slot descriptors.
        int slotIdx = 0;
        for (Expr analyticExpr : analyticExprs) {
            Type slotType = slots.get(slotIdx).getType();
            Preconditions.checkState(analyticExpr.getType().equals(slotType),
                    String.format("Analytic expr %s returns type %s but its analytic tuple " + "slot has type %s",
                            analyticExpr.toSql(), analyticExpr.getType().toString(), slotType.toString()));
            ++slotIdx;
        }
    }

    @Override
    public String debugString() {
        StringBuilder out = new StringBuilder(super.debugString());
        out.append(MoreObjects.toStringHelper(this)
                .add("analytic_exprs", Expr.debugString(analyticExprs))
                .add("smap", analyticTupleSmap.debugString())
                .toString());
        return out.toString();
    }

    @Override
    protected String tupleDebugName() {
        return "analytic-tuple";
    }

    @Override
    public AnalyticInfo clone() {
        return new AnalyticInfo(this);
    }
}
