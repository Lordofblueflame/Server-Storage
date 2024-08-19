import { Injectable } from '@angular/core';

@Injectable({
  providedIn: 'root',
})
export class SelectionService {
  selectedItems = new Set<number>();
  lastSelectedIndex: number | null = null;

  toggleSelection(index: number): void {
    if (this.selectedItems.has(index)) {
      this.selectedItems.delete(index);
    } else {
      this.selectedItems.add(index);
    }
    this.lastSelectedIndex = index;
  }

  clearAndSelect(index: number): void {
    this.selectedItems.clear();
    this.selectedItems.add(index);
    this.lastSelectedIndex = index;
  }

  selectRange(fromIndex: number, toIndex: number): void {
    const [start, end] = fromIndex < toIndex ? [fromIndex, toIndex] : [toIndex, fromIndex];
    for (let i = start; i <= end; i++) {
      this.selectedItems.add(i);
    }
  }

  isSelected(index: number): boolean {
    return this.selectedItems.has(index);
  }
}
