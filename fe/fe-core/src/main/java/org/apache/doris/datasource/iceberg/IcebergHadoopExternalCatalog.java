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

package org.apache.doris.datasource.iceberg;

import org.apache.doris.catalog.HdfsResource;
import org.apache.doris.datasource.CatalogProperty;
import org.apache.doris.datasource.property.PropertyConverter;
import org.apache.doris.datasource.property.storage.HdfsProperties;
import org.apache.doris.datasource.property.storage.StorageProperties;
import org.apache.doris.datasource.property.storage.StorageProperties.Type;

import com.google.common.base.Preconditions;
import org.apache.commons.lang3.StringUtils;
import org.apache.hadoop.conf.Configuration;
import org.apache.iceberg.CatalogProperties;
import org.apache.iceberg.hadoop.HadoopCatalog;

import java.util.Map;

public class IcebergHadoopExternalCatalog extends IcebergExternalCatalog {

    public IcebergHadoopExternalCatalog(long catalogId, String name, String resource, Map<String, String> props,
                                        String comment) {
        super(catalogId, name, comment);
        props = PropertyConverter.convertToMetaProperties(props);
        String warehouse = props.get(CatalogProperties.WAREHOUSE_LOCATION);
        Preconditions.checkArgument(StringUtils.isNotEmpty(warehouse),
                "Cannot initialize Iceberg HadoopCatalog because 'warehouse' must not be null or empty");
        catalogProperty = new CatalogProperty(resource, props);
        if (StringUtils.startsWith(warehouse, HdfsResource.HDFS_PREFIX)) {
            String nameService = StringUtils.substringBetween(warehouse, HdfsResource.HDFS_FILE_PREFIX, "/");
            if (StringUtils.isEmpty(nameService)) {
                throw new IllegalArgumentException("Unrecognized 'warehouse' location format"
                        + " because name service is required.");
            }
            catalogProperty.addProperty(HdfsResource.HADOOP_FS_NAME, HdfsResource.HDFS_FILE_PREFIX + nameService);
        }
    }

    @Override
    protected void initCatalog() {
        icebergCatalogType = ICEBERG_HADOOP;

        Configuration conf = getConfiguration();
        initS3Param(conf);
        // initialize hadoop catalog
        Map<String, String> catalogProperties = catalogProperty.getProperties();
        String warehouse = catalogProperty.getHadoopProperties().get(CatalogProperties.WAREHOUSE_LOCATION);
        HadoopCatalog hadoopCatalog = new HadoopCatalog();
        hadoopCatalog.setConf(conf);
        catalogProperties.put(CatalogProperties.WAREHOUSE_LOCATION, warehouse);

        // TODO: This is a temporary solution to support Iceberg with HDFS Kerberos authentication.
        // Because currently, DelegateFileIO only support hdfs file operation,
        // and all we want to solve is to use the hdfs file operation in Iceberg to support Kerberos authentication.
        // Later, we should always set FILE_IO_IMPL to DelegateFileIO for all kinds of storages.
        // So, here we strictly check the storage property, if only has one storage property and is kerberos hdfs,
        // then we will use this file io impl.
        Map<StorageProperties.Type, StorageProperties> storagePropertiesMap = catalogProperty.getStoragePropertiesMap();
        if (storagePropertiesMap.size() == 1) {
            HdfsProperties hdfsProperties = (HdfsProperties) storagePropertiesMap.get(Type.HDFS);
            if (hdfsProperties != null && hdfsProperties.isKerberos()) {
                catalogProperties.put(CatalogProperties.FILE_IO_IMPL,
                        "org.apache.doris.datasource.iceberg.fileio.DelegateFileIO");
            }
        }

        try {
            this.catalog = preExecutionAuthenticator.execute(() -> {
                hadoopCatalog.initialize(getName(), catalogProperties);
                return hadoopCatalog;
            });
        } catch (Exception e) {
            throw new RuntimeException("Hadoop catalog init error!", e);
        }
    }
}
