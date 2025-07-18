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

package org.apache.doris.load.loadv2;

import org.apache.doris.common.io.Writable;
import org.apache.doris.load.EtlStatus;
import org.apache.doris.load.FailMsg;
import org.apache.doris.transaction.TransactionState;
import org.apache.doris.transaction.TxnCommitAttachment;

import com.google.gson.annotations.SerializedName;

import java.util.HashMap;
import java.util.Map;

/**
 * This object will be created when job finished or cancelled.
 * It is used to edit the job final state.
 */
public class LoadJobFinalOperation extends TxnCommitAttachment implements Writable {
    @SerializedName(value = "id")
    private long id;
    @SerializedName(value = "ls")
    private EtlStatus loadingStatus = new EtlStatus();
    @SerializedName(value = "pro")
    private int progress;
    @SerializedName(value = "lst")
    private long loadStartTimestamp;
    @SerializedName(value = "ft")
    private long finishTimestamp;
    @SerializedName(value = "js")
    private JobState jobState;
    // optional
    @SerializedName(value = "fm")
    private FailMsg failMsg;
    // only used for copy into
    @SerializedName("cid")
    private String copyId = "";
    @SerializedName("lfp")
    private String loadFilePaths = "";
    @SerializedName("prop")
    private Map<String, String> properties = new HashMap<>();

    public LoadJobFinalOperation() {
        super(TransactionState.LoadJobSourceType.BATCH_LOAD_JOB);
    }

    public LoadJobFinalOperation(long id, EtlStatus loadingStatus, int progress, long loadStartTimestamp,
                                 long finishTimestamp, JobState jobState, FailMsg failMsg) {
        super(TransactionState.LoadJobSourceType.BATCH_LOAD_JOB);
        this.id = id;
        this.loadingStatus = loadingStatus;
        this.progress = progress;
        this.loadStartTimestamp = loadStartTimestamp;
        this.finishTimestamp = finishTimestamp;
        this.jobState = jobState;
        this.failMsg = failMsg;
    }

    public LoadJobFinalOperation(long id, EtlStatus loadingStatus, int progress, long loadStartTimestamp,
                                 long finishTimestamp, JobState jobState, FailMsg failMsg, String copyId,
                                 String loadFilePaths, Map<String, String> properties) {
        this(id, loadingStatus, progress, loadStartTimestamp, finishTimestamp, jobState, failMsg);
        this.copyId = copyId;
        this.loadFilePaths = loadFilePaths;
        this.properties = properties;
    }

    public long getId() {
        return id;
    }

    public EtlStatus getLoadingStatus() {
        return loadingStatus;
    }

    public int getProgress() {
        return progress;
    }

    public long getLoadStartTimestamp() {
        return loadStartTimestamp;
    }

    public long getFinishTimestamp() {
        return finishTimestamp;
    }

    public JobState getJobState() {
        return jobState;
    }

    public FailMsg getFailMsg() {
        return failMsg;
    }

    public String getCopyId() {
        return copyId;
    }

    public String getLoadFilePaths() {
        return loadFilePaths;
    }

    public Map<String, String> getProperties() {
        return properties;
    }

    @Override
    public String toString() {
        return "LoadJobEndOperation{"
                + "id=" + id
                + ", loadingStatus=" + loadingStatus
                + ", progress=" + progress
                + ", loadStartTimestamp=" + loadStartTimestamp
                + ", finishTimestamp=" + finishTimestamp
                + ", jobState=" + jobState
                + ", failMsg=" + failMsg
                + ", queryId=" + copyId
                + ", loadFilePaths=" + loadFilePaths
                + ", properties=" + properties
                + '}';
    }
}
