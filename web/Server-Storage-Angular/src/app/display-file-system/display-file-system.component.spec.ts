import { ComponentFixture, TestBed } from '@angular/core/testing';

import { DisplayFileSystemComponent } from './display-file-system.component';

describe('DisplayFileSystemComponent', () => {
  let component: DisplayFileSystemComponent;
  let fixture: ComponentFixture<DisplayFileSystemComponent>;

  beforeEach(async () => {
    await TestBed.configureTestingModule({
      imports: [DisplayFileSystemComponent]
    })
    .compileComponents();

    fixture = TestBed.createComponent(DisplayFileSystemComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });
});
