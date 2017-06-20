#ifndef XT_CONCAT_DATASET_H
#define XT_CONCAT_DATASET_H

#include "Dataset.h"

class ConcatDataset : public Dataset
{
public:
   ConcatDataset(std::vector<Dataset*>& datasets);
   virtual void getField(uint64_t idx, std::string& fieldkey, tlib::Tensor &field);
   virtual uint64_t size();
private:
   uint64_t binarySearch(uint64_t idx);
   std::vector<Dataset*>* datasets_;
   std::vector<uint64_t> beginindices_;
   std::vector<uint64_t> endindices_;
   uint64_t size_;
};

#endif
