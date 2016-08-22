#include <string.h>

#include "ch.h"
#include "hal.h"

#include "nand_ring.h"
#include "timeboot_u64.h"
#include "soft_crc.h"
#include "libnand.h"

/*
 * После запуска считаем, что предыдущий раз был неудачен по причине
 * отключения электричества, поэтому конец лога записываем заново:
 * 1) ощищаем следующий блок
 * 2) копируем заведомо целые страницы в него
 * 3) ощищаем предыдущий
 * 4) копируем страницы обратно
 *
 * На старте сканируем первые страницы блоков с нулевого по последний
 * 1) Последним считаем блок с максимальным номером
 * 2) Если номера совпадают - значит был прерван процесс восстановления: повторяем его.
 *
 * Форматирование - это простая очистка всех блоков
 *
 * Если при записи память обнаружила ошибку - перемещаем записанные данные
 * в новый блок, текущий помечаем, как сбойный
 *
 * Если ошибка обнаружилась при чтении - пытаемся скорректировать данные. Блок
 * НЕ бракуем.
 */

/*
 * Обновление от 16.08.2016
 *
 * mount()
 * 1) ищем последнюю записанную страницу
 * 1.1) пробегаем все блоки, начиная с первого исправного, читаем из нулевых
 *      страниц id и проверяем его на специальные значения. В случае
 *      битой контрольной суммы id считаем PAGE_ID_WASTED. Если ни одного
 *      валидного блока не обнаружено - возвращаем LAST_BLOCK_NOT_FOUND.
 * 1.2) если найден хотя бы один валидный блок - пробегаем в нем страницы
 *      с нулевой до первой невалидной. Возвращаем номер последней валидной.
 * 1.3) зануляем все оставшиеся страницы блока.
 * 1.4) очищаем следующий исправный блок и устанавливаем на него указатель.
 *
 * mkfs()
 * 1) ищет первый исправный блок, очищает его, возвращает его номер
 */

/*
 ******************************************************************************
 * DEFINES
 ******************************************************************************
 */
#define PAGE_ID_WASTED            (uint64_t)0x0 /* так же используется в случае сбоя CRC */
#define PAGE_ID_FIRST             (uint64_t)0x1

#define BLOCK_NOT_FOUND           0xFFFFFFFF
#define LAST_PAGE_NOT_FOUND       0xFFFFFFFF

#define MIN_RING_SIZE             64

#define SCRATCHPAD_SIZE           (2048 + 64)

/*
 ******************************************************************************
 * EXTERNS
 ******************************************************************************
 */

/*
 ******************************************************************************
 * PROTOTYPES
 ******************************************************************************
 */

/*
 ******************************************************************************
 * GLOBAL VARIABLES
 ******************************************************************************
 */

static uint8_t scratchpad[SCRATCHPAD_SIZE];

/*
 ******************************************************************************
 ******************************************************************************
 * LOCAL FUNCTIONS
 ******************************************************************************
 ******************************************************************************
 */

/**
 * @brief   Calculate total amount of available good blocks
 */
static uint32_t get_total_good(const NandRing *ring) {

  osalDbgCheck(NULL != ring->config);

  uint32_t ret = 0;
  NANDDriver *nandp = ring->config->nandp;
  const uint32_t start = ring->config->start_blk;
  const uint32_t len = ring->config->len;

  for (size_t b=0; b<len; b++) {
    if (! nandIsBad(nandp, start+b)) {
      ret++;
    }
  }

  return ret;
}

/**
 *
 */
static uint32_t next_good(const NandRing *ring, uint32_t current) {

  const uint32_t len   = ring->config->len;
  const uint32_t start = ring->config->start_blk;
  NANDDriver *nandp    = ring->config->nandp;
  uint32_t b = current;

  do {
    b++;
    if (b == (start + len)) {
      b = start;
    }
    if (! nandIsBad(nandp, b)) {
      return b;
    }
  } while (b != current); /* search wrapped without success */

  // TODO: check this value in calling functions
  return BLOCK_NOT_FOUND;
}

/**
 * @brief erase_next
 * @param ring
 * @return
 */
static uint32_t erase_next(NandRing *ring, uint32_t cur_blk) {

  NANDDriver *nandp = ring->config->nandp;
  uint8_t status = NAND_STATUS_FAILED;
  uint32_t blk = BLOCK_NOT_FOUND;

  do {
    blk = next_good(ring, cur_blk);
    status = nandErase(nandp, blk);
    if (nandFailed(status)) {
      nandMarkBad(nandp, blk);
    }
  } while (nandFailed(status));

  return blk;
}

/**
 * @brief crc algorithm for spare area
 * @param data
 * @param len
 * @return
 */
static uint32_t nand_ring_crc32(const uint8_t *data, size_t len) {
  return softcrc32(data, len, 0xFFFFFFFF);
}

/**
 *
 */
static uint32_t calc_spare_crc(const NandPageHeader *header) {

  const size_t len = sizeof(NandPageHeader) - sizeof(header->spare_crc);
  return nand_ring_crc32((const uint8_t *)header, len);
}

/**
 * @brief check_header_crc
 * @return
 */
static bool header_crc_valid(const NandPageHeader *header) {

  const uint32_t crc = calc_spare_crc(header);
  if (header->spare_crc == crc) {
    return true;
  }
  else {
    return false;
  }
}

/**
 *
 */
static void header2spare(uint8_t *buf, const NandPageHeader *header) {
  memcpy(buf, header, sizeof(NandPageHeader));
}

/**
 * @brief read_id
 * @param ring
 * @param blk
 * @param page
 * @return
 */
static uint64_t read_page_id(const NandRing *ring, uint32_t blk, uint32_t page) {

  NANDDriver *nandp = ring->config->nandp;
  const size_t pss = nandp->config->page_spare_size;
  uint8_t sparebuf[pss];
  const NandPageHeader *header = (const NandPageHeader *)sparebuf;

  nandReadPageSpare(nandp, blk, page, sparebuf, pss);

  if (! header_crc_valid(header)) {
    return PAGE_ID_WASTED;
  }
  else {
    return header->id;
  }
}

/**
 * @brief first_good
 * @param ring
 * @return
 */
static uint32_t first_good(const NandRing *ring) {
  return next_good(ring, ring->config->start_blk + ring->config->len - 1);
}

/**
 * @brief   Find last written block using brute force method starting
 *          from the first block of the ring
 * @return
 */
static uint32_t last_written_block(const NandRing *ring) {

  /* very first block of ring */
  const uint32_t first = first_good(ring);
  uint32_t last_blk = BLOCK_NOT_FOUND;
  uint64_t last_id  = PAGE_ID_FIRST;

  /* iterate over good blocks until block number wraps */
  uint32_t b = first;
  do {
    const uint64_t id = read_page_id(ring, b, 0);
    if (id >= last_id) {
      last_blk = b;
      last_id  = id;
    }
    b = next_good(ring, b);
  } while (b > first);

  return last_blk;
}

/**
 * @brief   Find last written page in the last written block
 * @param   ring
 * @param   last_blk
 * @return
 */
static uint32_t last_written_page(const NandRing *ring, uint32_t last_blk) {

  osalDbgCheck(BLOCK_NOT_FOUND != last_blk);

  uint64_t last_id = PAGE_ID_FIRST;
  uint32_t last_page = LAST_PAGE_NOT_FOUND;
  const size_t ppb = ring->config->nandp->config->pages_per_block;

  for (size_t page=0; page<ppb; page++) {
    const uint64_t id = read_page_id(ring, last_blk, page);
    if (id >= last_id) {
      last_page = page;
      last_id   = id;
    }
  }

  /* last page search must be called after last block search which must
     find at least one valid page */
  osalDbgCheck(LAST_PAGE_NOT_FOUND != last_page);

  return last_page;
}

/**
 * @brief Overwrites erased pages in last block.
 * @param ring
 * @param last_blk
 * @param last_page
 * @return
 */
static uint32_t close_prev_session(NandRing *ring, uint32_t last_blk,
                                   uint32_t last_page) {

  NANDDriver *nandp = ring->config->nandp;
  const size_t ppb = nandp->config->pages_per_block;
  const size_t pds = nandp->config->page_data_size;

  if (last_page != (ppb - 1)) {
    memset(scratchpad, 0, SCRATCHPAD_SIZE);
    memset(&scratchpad[pds], 0xFF, 2);

    for (size_t page=last_page; page<ppb; page++) {
      // TODO: check returned written status
      const uint8_t status = nandWritePageWhole(nandp, last_blk, page,
                                                scratchpad, SCRATCHPAD_SIZE);
      if (nandFailed(status)) {
        nandMarkBad(nandp, last_blk);
      }
    }
  }

  return erase_next(ring, last_blk);
}

/**
 *
 */
static uint32_t mkfs(NandRing *ring) {

  uint32_t cur_blk = first_good(ring);
  return erase_next(ring, cur_blk);
}

/**
 * @brief   Move data from failed block to new one and set bad mark in old.
 * @retval  New block number.
 */
static uint32_t block_data_rescue(NandRing *ring, uint32_t failed_blk,
                                  uint32_t failed_page) {

  NANDDriver *nandp = ring->config->nandp;
  uint32_t target_blk;
  uint8_t status = NAND_STATUS_FAILED;

  if (failed_page > 0) {
    RETRY:
    target_blk = erase_next(ring, ring->cur_blk);
    status = nandDataMove(nandp, failed_blk, target_blk, failed_page, scratchpad);
    if (nandFailed(status)) {
      nandMarkBad(nandp, target_blk);
      goto RETRY;
    }
  }
  else {
    target_blk = erase_next(ring, ring->cur_blk);
  }

  return target_blk;
}

/*
 ******************************************************************************
 * EXPORTED FUNCTIONS
 ******************************************************************************
 */

/**
 * @brief nandRingObjectInit
 * @param ring
 */
void nandRingObjectInit(NandRing *ring) {

  osalDbgCheck(NULL != ring);

  ring->config = NULL;
  ring->state = NAND_RING_UNINIT;
  ring->_dbg_fake_write_faled = 0;
  /* other fields will be initialized during start() */
}

/**
 * @brief nandRingStart
 * @param ring
 * @param config
 */
void nandRingStart(NandRing *ring, const NandRingConfig *config) {

  osalDbgCheck((NULL != ring) && (NULL != config) && (NULL != config->nandp));
  osalDbgAssert(NAND_READY == config->nandp->state,
                "NAND must be started externally");
  osalDbgAssert((config->start_blk + config->len) <= config->nandp->config->blocks,
                "NAND overflow");
  osalDbgCheck(config->len >= MIN_RING_SIZE);
  osalDbgAssert(sizeof(NandPageHeader) <= config->nandp->config->page_spare_size,
                "Not enough room in spare area");

  ring->config = config;
  ring->state = NAND_RING_IDLE;
  /* other fields will be initialized during mount() */
}

/**
 *
 */
bool nandRingMount(NandRing *ring) {

  osalDbgCheck(NULL != ring);
  osalDbgCheck(NAND_RING_IDLE == ring->state);

  if (get_total_good(ring) < (MIN_RING_SIZE / 2)) {
    return OSAL_FAILED;
  }

  uint32_t last_blk  = last_written_block(ring);
  if (BLOCK_NOT_FOUND == last_blk) {
    ring->cur_blk = mkfs(ring);
    ring->cur_page = 0;
    ring->cur_id = PAGE_ID_FIRST;
  }
  else {
    uint32_t last_page = last_written_page(ring, last_blk);
    uint64_t last_id   = read_page_id(ring, last_blk, last_page);

    ring->cur_blk = close_prev_session(ring, last_blk, last_page);
    ring->cur_page = 0;
    ring->cur_id = last_id + 1;
  }

  ring->state = NAND_RING_MOUNTED;
  return OSAL_SUCCESS;
}

/**
 * @brief   Flush data to NAND page and seal it using spare area.
 * @note    Buffer must be the same size as page.
 * @note    This function able to write only single whole page at time.
 */
void nandRingWritePage(NandRing *ring, const uint8_t *data) {

  osalDbgCheck((NULL != data) && (NULL != ring));
  osalDbgCheck(NAND_RING_MOUNTED == ring->state);

  NANDDriver *nandp = ring->config->nandp;
  const size_t ppb = nandp->config->pages_per_block;
  const size_t pss = nandp->config->page_spare_size;
  const size_t pds = nandp->config->page_data_size;
  uint32_t page_ecc;
  uint8_t status = NAND_STATUS_FAILED;

  /* write page data */
RETRY:
  status = nandWritePageData(nandp, ring->cur_blk, ring->cur_page,
                             data, pds, &page_ecc);
  if (nandFailed(status)) {
    nandMarkBad(nandp, ring->cur_blk);
    ring->cur_blk = block_data_rescue(ring, ring->cur_blk, ring->cur_page);
    goto RETRY;
  }

  /* seal page using spare area */
  NandPageHeader header;
  uint8_t sparebuf[pss];
  header.page_ecc       = page_ecc;
  header.bad_mark       = 0xFFFF;
  header.id             = ring->cur_id;
  header.utc_correction = ring->utc_correction;
  header.time_boot_uS   = timebootU64();
  header.spare_crc      = calc_spare_crc(&header);
  header2spare(sparebuf, &header);
  status = nandWritePageSpare(nandp, ring->cur_blk, ring->cur_page, sparebuf, pss);
  if (nandFailed(status)) {
    nandMarkBad(nandp, ring->cur_blk);
    ring->cur_blk = block_data_rescue(ring, ring->cur_blk, ring->cur_page);
    goto RETRY;
  }

  /* prepare next iteration */
  ring->cur_id++;
  ring->cur_page++;
  if (ring->cur_page == ppb) {
    ring->cur_page = 0;
    ring->cur_blk = erase_next(ring, ring->cur_blk);
  }
}

/**
 * @brief   Calculate total amount of available good blocks
 */
uint32_t nandRingTotalGood(const NandRing *ring) {

  osalDbgCheck(NULL != ring);
  osalDbgCheck(NAND_RING_MOUNTED == ring->state);

  return get_total_good(ring);
}

/**
 * @brief nandRingSetUtcCorrection
 * @param correction
 */
void nandRingSetUtcCorrection(uint32_t correction) {
  (void)correction;
  osalSysHalt("Unrealized yet");
}

/**
 * @brief nandRingSearchSessions
 * @param result
 * @param max_sessions
 * @return
 */
size_t nandRingSearchSessions(RingSession *result, size_t max_sessions) {
  (void)result;
  (void)max_sessions;
  osalSysHalt("Unrealized yet");
  return 0;
}

/**
 * @brief nandRingUmount
 * @param ring
 */
void nandRingUmount(NandRing *ring) {

  osalDbgCheck(NULL != ring);
  ring->state = NAND_RING_IDLE;
}

/**
 * @brief nandRingStop
 * @param ring
 */
void nandRingStop(NandRing *ring) {

  osalDbgCheck(NULL != ring);
  osalDbgCheck(NAND_RING_IDLE == ring->state);

  ring->state = NAND_RING_STOP;
  ring->config = NULL;
}